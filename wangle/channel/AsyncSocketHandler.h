/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#pragma once

#include <folly/io/IOBuf.h>
#include <folly/io/IOBufQueue.h>
#include <folly/io/async/AsyncSocket.h>
#include <folly/io/async/EventBase.h>
#include <folly/io/async/EventBaseManager.h>
#include <wangle/channel/Handler.h>

namespace wangle {

// This handler may only be used in a single Pipeline
class AsyncSocketHandler : public wangle::BytesToBytesHandler,
                           public folly::AsyncTransport::ReadCallback {
 public:
  explicit AsyncSocketHandler(std::shared_ptr<folly::AsyncTransport> socket)
      : socket_(std::move(socket)) {}

  AsyncSocketHandler(AsyncSocketHandler&&) = default;

  ~AsyncSocketHandler() override {
    detachReadCallback();

    if (socket_) {
      auto evb = socket_->getEventBase();
      if (evb) {
        evb->runImmediatelyOrRunInEventBaseThreadAndWait(
            [s = std::move(socket_)]() mutable { s.reset(); });
      }
    }
  }

  void attachReadCallback() {
    socket_->setReadCB(socket_->good() ? this : nullptr);
  }

  void detachReadCallback() {
    if (socket_ && socket_->getReadCallback() == this) {
      socket_->setReadCB(nullptr);
    }
    auto ctx = getContext();
    if (ctx && !firedInactive_) {
      firedInactive_ = true;
      ctx->fireTransportInactive();
    }
  }

  void attachEventBase(folly::EventBase* eventBase) {
    if (eventBase && !socket_->getEventBase()) {
      socket_->attachEventBase(eventBase);
    }
  }

  void detachEventBase() {
    detachReadCallback();
    if (socket_->getEventBase()) {
      socket_->detachEventBase();
    }
  }

  void transportActive(Context* ctx) override {
    // QM: async_socket 到 pipeline 的关键节点
    // 传入的是 socket 信息
    // 在这里进行回调的注册
    ctx->getPipeline()->setTransport(socket_);
    attachReadCallback();
    firedInactive_ = false;
    ctx->fireTransportActive();
  }

  void transportInactive(Context* ctx) override {
    // detachReadCallback invokes fireTransportInactive() if the transport
    // is currently active.
    detachReadCallback();
    ctx->getPipeline()->setTransport(nullptr);
  }

  void detachPipeline(Context*) override {
    detachReadCallback();
  }

  folly::Future<folly::Unit> write(
      Context* ctx,
      std::unique_ptr<folly::IOBuf> buf) override {
    refreshTimeout();
    if (UNLIKELY(!buf)) {
      return folly::makeFuture();
    }

    if (!socket_->good()) {
      VLOG(5) << "socket is closed in write()";
      return folly::makeFuture<folly::Unit>(folly::AsyncSocketException(
          folly::AsyncSocketException::AsyncSocketExceptionType::NOT_OPEN,
          "socket is closed in write()"));
    }

    auto cb = new WriteCallback();
    auto future = cb->promise_.getFuture();
    // QM: 这里如何保证在 eventbase thread
    // 是否真的需要: 看AsyncSocket 里面是需要的
    // 这里没有保证: 所以如果要切CPU 线程池, 需要加 EventBaseHandler
    socket_->writeChain(cb, std::move(buf), ctx->getWriteFlags());
    return future;
  }

  folly::Future<folly::Unit> writeException(
      Context* ctx,
      folly::exception_wrapper) override {
    return shutdown(ctx, true);
  }

  folly::Future<folly::Unit> close(Context* ctx) override {
    bool shutdownWriteOnly =
        isSet(ctx->getWriteFlags(), folly::WriteFlags::WRITE_SHUTDOWN);
    if (shutdownWriteOnly) {
      socket_->shutdownWrite();
      return folly::makeFuture();
    } else {
      return shutdown(ctx, false);
    }
  }

  // Must override to avoid warnings about hidden overloaded virtual due to
  // AsyncSocket::ReadCallback::readEOF()
  void readEOF(Context* ctx) override {
    ctx->fireReadEOF();
  }

  void getReadBuffer(void** bufReturn, size_t* lenReturn) override {
    const auto readBufferSettings = getContext()->getReadBufferSettings();
    // QM: 每次 socket 可以读了, 就预分配一块内存（默认长度 2048）, 这里只返回指针
    // QM: 怎么保证不写越界? 因为这里返回了可以写的长度啊
    const auto ret = bufQueue_.preallocate(
        readBufferSettings.first, readBufferSettings.second);
    *bufReturn = ret.first;
    *lenReturn = ret.second;
  }

  void readDataAvailable(size_t len) noexcept override {
    // QM: 这里是 async_socket 可写事件的回调,
    // 此处调用时, socket 读已经完成, 内容已经写到buf queue里面了
    refreshTimeout();
    // QM: 因为从socket 中读到了 len 长度, 所以告诉 bufqueue 读了多少,
    bufQueue_.postallocate(len);
    getContext()->fireRead(bufQueue_);
  }

  void readEOF() noexcept override {
    getContext()->fireReadEOF();
  }

  void readErr(const folly::AsyncSocketException& ex) noexcept override {
    getContext()->fireReadException(
        folly::make_exception_wrapper<folly::AsyncSocketException>(ex));
  }

 private:
  void refreshTimeout() {
    auto manager = getContext()->getPipeline()->getPipelineManager();
    if (manager) {
      manager->refreshTimeout();
    }
  }

  folly::Future<folly::Unit> shutdown(Context* ctx, bool closeWithReset) {
    if (socket_) {
      detachReadCallback();
      if (closeWithReset) {
        socket_->closeWithReset();
      } else {
        socket_->closeNow();
      }
    }
    if (!pipelineDeleted_) {
      pipelineDeleted_ = true;
      ctx->getPipeline()->deletePipeline();
    }
    return folly::makeFuture();
  }

  class WriteCallback : private folly::AsyncTransport::WriteCallback {
    void writeSuccess() noexcept override {
      promise_.setValue();
      delete this;
    }

    void writeErr(
        size_t /* bytesWritten */,
        const folly::AsyncSocketException& ex) noexcept override {
      promise_.setException(ex);
      delete this;
    }

   private:
    friend class AsyncSocketHandler;
    folly::Promise<folly::Unit> promise_;
  };

  // QM: 这里可以优化下? 每次请求重新创建 bufQueue_ 比较费
  folly::IOBufQueue bufQueue_{folly::IOBufQueue::cacheChainLength()};
  std::shared_ptr<folly::AsyncTransport> socket_{nullptr};
  bool firedInactive_{false};
  bool pipelineDeleted_{false};
};

} // namespace wangle
