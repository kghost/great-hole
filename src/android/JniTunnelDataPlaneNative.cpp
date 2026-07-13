#include "ConnectionTracker.hpp"
#include "info_kghost_android_hole_vpn_dataplane_JniTunnelDataPlaneNative.h"

#include <android/log.h>
#include <ares.h>
#include <atomic>
#include <future>
#include <jni.h>
#include <memory>
#include <optional>
#include <string>
#include <thread>
#include <unistd.h>
#include <utility>
#include <vector>

#include <boost/asio.hpp>
#include <boost/log/core.hpp>
#include <boost/log/expressions.hpp>
#include <boost/log/sinks/basic_sink_backend.hpp>
#include <boost/log/sinks/sync_frontend.hpp>
#include <boost/log/trivial.hpp>
#include <boost/make_shared.hpp>

#include <EventQueue.hpp>
#include <MoveOnlyFunction.hpp>

#include "Asio.hpp"
#include "Coroutine.hpp"
#include "Utils.hpp"
#include "Utils/Overload.hpp"

#include "TunnelDataPlane.hpp"

static JavaVM* g_JavaVM = nullptr;
static jclass g_CallbacksClass = nullptr;
static jmethodID g_MidFindTunnelForFlow = nullptr;
static jmethodID g_MidOnTunnelStateChanged = nullptr;
static jmethodID g_MidOnVpnStateChanged = nullptr;

namespace gh {

class JniSelector;

class JniSession : public DataPlaneCallbacks {
public:
  using Task = Omni::Fiber::move_only_function<Omni::Fiber::Coroutine<void>(TunnelDataPlane&, bool&)>;

  explicit JniSession(JNIEnv* env, jobject callbacks, jobject connectivityManager);
  ~JniSession() override;

  JniSession(const JniSession&) = delete;
  JniSession& operator=(const JniSession&) = delete;
  JniSession(JniSession&&) = delete;
  JniSession& operator=(JniSession&&) = delete;

  static JNIEnv* GetEnv() {
    JNIEnv* env = nullptr;
    jint res = g_JavaVM->GetEnv((void**)&env, JNI_VERSION_1_6);
    if (res == JNI_EDETACHED) {
      g_JavaVM->AttachCurrentThreadAsDaemon(&env, nullptr);
    }
    return env;
  }

  void Start(int tunFd, int mtu, std::vector<char> encryptionKey);
  void MigrateTun(int tunFd);
  void Stop();
  jlong AddEndpoint(const UdpDynMux::PskType& psk, const std::string& address);
  void RemoveEndpoint(jlong handle);
  void StartEndpoint(jlong handle);
  void StopEndpoint(jlong handle);

  jobject GetCallbacks() const { return _Callbacks; }
  TunnelDataPlane* GetDataPlane() const { return _DataPlane; }

  bool ProtectSocket(int fd);
  void OnVpnStateChanged(TunnelState state, const std::string& message) override;
  void OnTunnelStateChanged(const std::shared_ptr<VpnClientMultiChannelSession>& session, TunnelState state,
                            const std::string& error) override;
  std::optional<VpnTrafficStats> GetTrafficStats(jlong endpointHandle);

  template <typename Func> void PostTask(Func&& func) {
    _IoContext.post([this, func = std::forward<Func>(func)]() mutable {
      _TaskQueue.Push([func = std::move(func)](TunnelDataPlane& dp, bool& stop) -> Omni::Fiber::Coroutine<void> {
        co_await func(dp, stop);
      });
    });
  }

private:
  jobject _Callbacks = nullptr;
  jobject _ConnectivityManager = nullptr;
  boost::asio::io_context _IoContext;
  std::thread _Thread;

  std::unique_ptr<JniSelector> _Selector;
  Omni::Fiber::EventQueue<Task> _TaskQueue;
  TunnelDataPlane* _DataPlane = nullptr;

  std::atomic<bool> _Stopped{false};
};

class JniSelector : public ConnectionTracker::Selector {
public:
  explicit JniSelector(JniSession& session) : _Session(session) {}
  ~JniSelector() override = default;

  std::shared_ptr<ConnectionMark> operator()(const ConnectionTracker::ConnectionKey& key) override;

private:
  std::shared_ptr<ConnectionMark> FindTunnel(int protocol, const boost::asio::ip::address& localAddr,
                                             uint16_t localPort, const boost::asio::ip::address& remoteAddr,
                                             uint16_t remotePort) const;

  JniSession& _Session;
};

// JniSelector Implementation

std::shared_ptr<ConnectionMark> JniSelector::FindTunnel(int protocol, const boost::asio::ip::address& localAddr,
                                                        uint16_t localPort, const boost::asio::ip::address& remoteAddr,
                                                        uint16_t remotePort) const {

  JNIEnv* env = JniSession::GetEnv();
  if (!env || !_Session.GetCallbacks()) {
    return std::make_unique<VpnClientMultiChannel::Mark>(VpnClientMultiChannel::Mark::Discard{});
  }

  jbyteArray localBytes = env->NewByteArray(16);
  if (!localBytes || env->ExceptionCheck()) {
    BOOST_LOG_TRIVIAL(warning) << "JNI exception or allocation failure for localBytes in FindTunnel";
    if (env->ExceptionCheck()) {
      env->ExceptionClear();
    }
    return std::make_unique<VpnClientMultiChannel::Mark>(VpnClientMultiChannel::Mark::Discard{});
  }
  auto v6Local = MapToV6(localAddr).to_bytes();
  env->SetByteArrayRegion(localBytes, 0, 16, reinterpret_cast<const jbyte*>(v6Local.data()));

  jbyteArray remoteBytes = env->NewByteArray(16);
  if (!remoteBytes || env->ExceptionCheck()) {
    BOOST_LOG_TRIVIAL(warning) << "JNI exception or allocation failure for remoteBytes in FindTunnel";
    if (env->ExceptionCheck()) {
      env->ExceptionClear();
    }
    env->DeleteLocalRef(localBytes);
    return std::make_unique<VpnClientMultiChannel::Mark>(VpnClientMultiChannel::Mark::Discard{});
  }
  auto v6Remote = MapToV6(remoteAddr).to_bytes();
  env->SetByteArrayRegion(remoteBytes, 0, 16, reinterpret_cast<const jbyte*>(v6Remote.data()));

  jlong endpointHandle = env->CallLongMethod(_Session.GetCallbacks(), g_MidFindTunnelForFlow, protocol, localBytes,
                                             static_cast<jint>(localPort), remoteBytes, static_cast<jint>(remotePort));
  if (env->ExceptionCheck()) {
    BOOST_LOG_TRIVIAL(warning) << "JNI exception in FindTunnel (findTunnelForFlow) for protocol " << protocol;
    env->ExceptionDescribe();
    env->ExceptionClear();
    endpointHandle = 0;
  }

  env->DeleteLocalRef(localBytes);
  env->DeleteLocalRef(remoteBytes);

  if (endpointHandle != 0) {
    auto dp = _Session.GetDataPlane();
    if (dp) {
      auto session = dp->FindSessionByHandle(reinterpret_cast<VpnClientMultiChannelSession*>(endpointHandle));
      if (session) {
        return std::make_unique<VpnClientMultiChannel::Mark>(std::move(session));
      }
    }
  }
  return std::make_unique<VpnClientMultiChannel::Mark>(VpnClientMultiChannel::Mark::Discard{});
}

std::shared_ptr<ConnectionMark> JniSelector::operator()(const ConnectionTracker::ConnectionKey& key) {
  return std::visit(Overload{[this](const ConnectionTracker::Ip4TcpKey& k) {
                               return FindTunnel(6, k.LocalAddress, k.LocalPort, k.RemoteAddress, k.RemotePort);
                             },
                             [this](const ConnectionTracker::Ip6TcpKey& k) {
                               return FindTunnel(6, k.LocalAddress, k.LocalPort, k.RemoteAddress, k.RemotePort);
                             },
                             [this](const ConnectionTracker::Ip4UdpKey& k) {
                               return FindTunnel(17, k.LocalAddress, k.LocalPort, k.RemoteAddress, k.RemotePort);
                             },
                             [this](const ConnectionTracker::Ip6UdpKey& k) {
                               return FindTunnel(17, k.LocalAddress, k.LocalPort, k.RemoteAddress, k.RemotePort);
                             },
                             [this](const ConnectionTracker::IcmpKey& k) {
                               return FindTunnel(1, k.LocalAddress, k.Id, k.RemoteAddress, 0);
                             },
                             [this](const ConnectionTracker::Icmp6Key& k) {
                               return FindTunnel(58, k.LocalAddress, k.Id, k.RemoteAddress, 0);
                             }},
                    key);
}

// JniSession Implementation

JniSession::JniSession(JNIEnv* env, jobject callbacks, jobject connectivityManager) {
  _Callbacks = env->NewGlobalRef(callbacks);
  _ConnectivityManager = env->NewGlobalRef(connectivityManager);

  int res = ares_library_init_android(_ConnectivityManager);
  if (res != ARES_SUCCESS) {
    BOOST_LOG_TRIVIAL(warning) << "ares_library_init_android failed: " << res;
  }
  _Thread = std::thread([this]() {
    JNIEnv* localEnv = nullptr;
    JavaVMAttachArgs args;
    args.version = JNI_VERSION_1_6;
    args.name = (char*)"great_hole_worker";
    args.group = nullptr;
    g_JavaVM->AttachCurrentThreadAsDaemon(&localEnv, &args);

    auto ioExecutor = _IoContext.get_executor();
    Omni::Fiber::AsioExecutor executor(ioExecutor);
    Omni::Fiber::Manager manager(executor);

    _Selector = std::make_unique<JniSelector>(*this);

    manager.SpawnRoot("root", [this]() -> Omni::Fiber::Coroutine<void> {
      TunnelDataPlane dp(_IoContext.get_executor(), *_Selector, *this);
      _DataPlane = &dp;

      bool stop = false;
      while (!stop) {
        co_await _TaskQueue;
        while (!_TaskQueue.IsEmpty()) {
          auto task = _TaskQueue.PopFront();
          co_await task(dp, stop);
        }
      }

      _DataPlane = nullptr;
      co_return;
    });

    _IoContext.run();
  });
}

JniSession::~JniSession() {
  Stop();
  JNIEnv* env = GetEnv();
  if (env) {
    if (_Callbacks) {
      env->DeleteGlobalRef(_Callbacks);
    }
    if (_ConnectivityManager) {
      env->DeleteGlobalRef(_ConnectivityManager);
    }
  }
}

void JniSession::Start(int tunFd, int mtu, std::vector<char> encryptionKey) {
  PostTask([tunFd, mtu, encryptionKey = std::move(encryptionKey)](TunnelDataPlane& dp,
                                                                  bool& stop) -> Omni::Fiber::Coroutine<void> {
    co_await dp.Start(tunFd, mtu, std::move(encryptionKey));
    co_return;
  });
}

void JniSession::MigrateTun(int tunFd) {
  PostTask([tunFd](TunnelDataPlane& dp, bool& stop) -> Omni::Fiber::Coroutine<void> {
    co_await dp.MigrateTun(tunFd);
    co_return;
  });
}

void JniSession::Stop() {
  bool expected = false;
  if (!_Stopped.compare_exchange_strong(expected, true)) {
    return;
  }

  std::promise<void> promise;
  auto future = promise.get_future();

  PostTask([&promise](TunnelDataPlane& dp, bool& stop) -> Omni::Fiber::Coroutine<void> {
    co_await dp.Stop();
    stop = true;
    promise.set_value();
    co_return;
  });

  future.get();

  if (_Thread.joinable()) {
    _Thread.join();
  }
}

jlong JniSession::AddEndpoint(const UdpDynMux::PskType& psk, const std::string& address) {
  std::promise<jlong> promise;
  auto future = promise.get_future();

  PostTask([&promise, psk, address](TunnelDataPlane& dp, bool& stop) -> Omni::Fiber::Coroutine<void> {
    auto session = co_await dp.AddEndpoint(psk, address);
    promise.set_value(reinterpret_cast<jlong>(session.get()));
    co_return;
  });

  return future.get();
}

void JniSession::RemoveEndpoint(jlong handle) {
  std::promise<void> promise;
  auto future = promise.get_future();

  PostTask([&promise, handle](TunnelDataPlane& dp, bool& stop) -> Omni::Fiber::Coroutine<void> {
    co_await dp.RemoveEndpoint(dp.FindSessionByHandle(reinterpret_cast<VpnClientMultiChannelSession*>(handle)));
    promise.set_value();
    co_return;
  });

  future.get();
}

void JniSession::StartEndpoint(jlong handle) {
  // Negotiation is automatically started on setup
}

void JniSession::StopEndpoint(jlong handle) {
  // Managed by remove or stop
}

void JniSession::OnVpnStateChanged(TunnelState state, const std::string& message) {
  if (!_Callbacks) {
    return;
  }
  JNIEnv* env = GetEnv();
  jstring msgStr = !message.empty() ? env->NewStringUTF(message.c_str()) : nullptr;
  if (env->ExceptionCheck()) {
    BOOST_LOG_TRIVIAL(warning) << "JNI exception in OnVpnStateChanged (NewStringUTF)";
    env->ExceptionClear();
  }
  env->CallVoidMethod(_Callbacks, g_MidOnVpnStateChanged, static_cast<jint>(state), msgStr);
  if (env->ExceptionCheck()) {
    BOOST_LOG_TRIVIAL(warning) << "JNI exception in OnVpnStateChanged (onVpnStateChanged)";
    env->ExceptionDescribe();
    env->ExceptionClear();
  }
  if (msgStr) {
    env->DeleteLocalRef(msgStr);
  }
}

void JniSession::OnTunnelStateChanged(const std::shared_ptr<VpnClientMultiChannelSession>& session, TunnelState state,
                                      const std::string& error) {
  if (!_Callbacks) {
    return;
  }
  JNIEnv* env = GetEnv();
  jstring errStr = !error.empty() ? env->NewStringUTF(error.c_str()) : nullptr;
  if (env->ExceptionCheck()) {
    BOOST_LOG_TRIVIAL(warning) << "JNI exception in OnTunnelStateChanged (NewStringUTF)";
    env->ExceptionClear();
  }
  env->CallVoidMethod(_Callbacks, g_MidOnTunnelStateChanged, reinterpret_cast<jlong>(session.get()),
                      static_cast<jint>(std::to_underlying(state)), errStr);
  if (env->ExceptionCheck()) {
    BOOST_LOG_TRIVIAL(warning) << "JNI exception in OnTunnelStateChanged (onTunnelStateChanged)";
    env->ExceptionDescribe();
    env->ExceptionClear();
  }
  if (errStr) {
    env->DeleteLocalRef(errStr);
  }
}

std::optional<VpnTrafficStats> JniSession::GetTrafficStats(jlong endpointHandle) {
  std::promise<std::optional<VpnTrafficStats>> promise;
  auto future = promise.get_future();

  PostTask([&promise, endpointHandle](TunnelDataPlane& dp, bool& stop) -> Omni::Fiber::Coroutine<void> {
    auto* session = reinterpret_cast<VpnClientMultiChannelSession*>(endpointHandle);
    promise.set_value(dp.GetTrafficStats(dp.FindSessionByHandle(session)));
    co_return;
  });

  return future.get();
}

} // namespace gh

static gh::JniSession* GetSession(jlong handle) { return reinterpret_cast<gh::JniSession*>(handle); }

namespace {

class AndroidLogBackend
    : public boost::log::sinks::basic_formatted_sink_backend<char, boost::log::sinks::synchronized_feeding> {
public:
  void consume(const boost::log::record_view& rec, const string_type& formattedMessage) {
    android_LogPriority priority = ANDROID_LOG_INFO;
    if (auto severity = rec[boost::log::trivial::severity]) {
      switch (*severity) {
      case boost::log::trivial::trace:
        priority = ANDROID_LOG_VERBOSE;
        break;
      case boost::log::trivial::debug:
        priority = ANDROID_LOG_DEBUG;
        break;
      case boost::log::trivial::info:
        priority = ANDROID_LOG_INFO;
        break;
      case boost::log::trivial::warning:
        priority = ANDROID_LOG_WARN;
        break;
      case boost::log::trivial::error:
        priority = ANDROID_LOG_ERROR;
        break;
      case boost::log::trivial::fatal:
        priority = ANDROID_LOG_FATAL;
        break;
      }
    }
    __android_log_write(priority, "GreatHoleNDK", formattedMessage.c_str());
  }
};

void SetupLogging() {
  using SinkType = boost::log::sinks::synchronous_sink<AndroidLogBackend>;
  auto sink = boost::make_shared<SinkType>();
  boost::log::core::get()->add_sink(sink);
}

} // namespace

JNIEXPORT jint JNICALL JNI_OnLoad(JavaVM* vm, void* reserved) {
  g_JavaVM = vm;
  JNIEnv* env = nullptr;
  if (vm->GetEnv((void**)&env, JNI_VERSION_1_6) != JNI_OK) {
    return JNI_ERR;
  }

  SetupLogging();

  ares_library_init_jvm(vm);

  jclass localCallbacksClass = env->FindClass("info/kghost/android_hole/vpn/dataplane/TunnelDataPlaneCallbacks");
  if (!localCallbacksClass) {
    return JNI_ERR;
  }
  g_CallbacksClass = (jclass)env->NewGlobalRef(localCallbacksClass);

  g_MidFindTunnelForFlow = env->GetMethodID(g_CallbacksClass, "findTunnelForFlow", "(I[BI[BI)J");
  g_MidOnTunnelStateChanged = env->GetMethodID(g_CallbacksClass, "onTunnelStateChanged", "(JILjava/lang/String;)V");
  g_MidOnVpnStateChanged = env->GetMethodID(g_CallbacksClass, "onVpnStateChanged", "(ILjava/lang/String;)V");

  if (!g_MidFindTunnelForFlow || !g_MidOnTunnelStateChanged || !g_MidOnVpnStateChanged) {
    return JNI_ERR;
  }

  return JNI_VERSION_1_6;
}

JNIEXPORT void JNICALL JNI_OnUnload(JavaVM* vm, void* reserved) {
  JNIEnv* env = nullptr;
  if (vm->GetEnv((void**)&env, JNI_VERSION_1_6) == JNI_OK) {
    if (g_CallbacksClass) {
      env->DeleteGlobalRef(g_CallbacksClass);
    }
  }
}

extern "C" {

JNIEXPORT jlong JNICALL Java_info_kghost_android_1hole_vpn_dataplane_JniTunnelDataPlaneNative_nativeCreate(
    JNIEnv* env, jclass clazz, jobject callbacks, jobject connectivityManager) {
  auto* session = new gh::JniSession(env, callbacks, connectivityManager);
  return reinterpret_cast<jlong>(session);
}

JNIEXPORT void JNICALL Java_info_kghost_android_1hole_vpn_dataplane_JniTunnelDataPlaneNative_nativeStart(
    JNIEnv* env, jclass clazz, jlong sessionHandle, jint tunFd, jint mtu, jbyteArray encryptionKey) {
  auto session = GetSession(sessionHandle);
  if (session) {
    if (!encryptionKey) {
      BOOST_LOG_TRIVIAL(error) << "nativeStart: encryptionKey is null";
      return;
    }
    jsize len = env->GetArrayLength(encryptionKey);
    std::vector<char> key(len);
    env->GetByteArrayRegion(encryptionKey, 0, len, reinterpret_cast<jbyte*>(key.data()));
    session->Start(tunFd, mtu, std::move(key));
  }
}

JNIEXPORT void JNICALL Java_info_kghost_android_1hole_vpn_dataplane_JniTunnelDataPlaneNative_nativeMigrateTun(
    JNIEnv* env, jclass clazz, jlong sessionHandle, jint tunFd) {
  auto session = GetSession(sessionHandle);
  if (session) {
    session->MigrateTun(tunFd);
  } else {
    ::close(tunFd);
  }
}

JNIEXPORT jlong JNICALL Java_info_kghost_android_1hole_vpn_dataplane_JniTunnelDataPlaneNative_nativeAddEndpoint(
    JNIEnv* env, jclass clazz, jlong sessionHandle, jbyteArray psk, jstring address) {
  auto session = GetSession(sessionHandle);
  if (session) {
    if (!psk) {
      BOOST_LOG_TRIVIAL(error) << "nativeAddEndpoint: psk is null";
      return 0;
    }

    gh::UdpDynMux::PskType pskArray{};
    jsize len = env->GetArrayLength(psk);
    if (len == 16) {
      env->GetByteArrayRegion(psk, 0, 16, reinterpret_cast<jbyte*>(pskArray.data()));
    } else {
      BOOST_LOG_TRIVIAL(error) << "Invalid PSK length: " << len << " (expected 16)";
      return 0;
    }

    const char* addressChars = env->GetStringUTFChars(address, nullptr);
    std::string addressStr(addressChars);
    env->ReleaseStringUTFChars(address, addressChars);

    return session->AddEndpoint(pskArray, addressStr);
  }
  return 0;
}

JNIEXPORT void JNICALL Java_info_kghost_android_1hole_vpn_dataplane_JniTunnelDataPlaneNative_nativeRemoveEndpoint(
    JNIEnv* env, jclass clazz, jlong sessionHandle, jlong endpointHandle) {
  auto session = GetSession(sessionHandle);
  if (session) {
    session->RemoveEndpoint(endpointHandle);
  }
}

JNIEXPORT void JNICALL Java_info_kghost_android_1hole_vpn_dataplane_JniTunnelDataPlaneNative_nativeStartEndpoint(
    JNIEnv* env, jclass clazz, jlong sessionHandle, jlong endpointHandle) {
  auto session = GetSession(sessionHandle);
  if (session) {
    session->StartEndpoint(endpointHandle);
  }
}

JNIEXPORT void JNICALL Java_info_kghost_android_1hole_vpn_dataplane_JniTunnelDataPlaneNative_nativeStopEndpoint(
    JNIEnv* env, jclass clazz, jlong sessionHandle, jlong endpointHandle) {
  auto session = GetSession(sessionHandle);
  if (session) {
    session->StopEndpoint(endpointHandle);
  }
}

JNIEXPORT jboolean JNICALL Java_info_kghost_android_1hole_vpn_dataplane_JniTunnelDataPlaneNative_nativeGetTrafficStats(
    JNIEnv* env, jclass clazz, jlong sessionHandle, jlong endpointHandle, jobject stats) {
  auto session = GetSession(sessionHandle);
  if (!session || !stats) {
    return JNI_FALSE;
  }

  auto res = session->GetTrafficStats(endpointHandle);
  if (!res.has_value()) {
    return JNI_FALSE;
  }

  jclass statsClass = env->GetObjectClass(stats);
  if (!statsClass) {
    return JNI_FALSE;
  }

  jfieldID txBytesField = env->GetFieldID(statsClass, "txBytes", "J");
  jfieldID rxBytesField = env->GetFieldID(statsClass, "rxBytes", "J");
  jfieldID txPacketsField = env->GetFieldID(statsClass, "txPackets", "J");
  jfieldID rxPacketsField = env->GetFieldID(statsClass, "rxPackets", "J");
  jfieldID rttMsField = env->GetFieldID(statsClass, "rttMs", "J");
  if (!txBytesField || !rxBytesField || !txPacketsField || !rxPacketsField || !rttMsField) {
    return JNI_FALSE;
  }

  env->SetLongField(stats, txBytesField, static_cast<jlong>(res->BackwardBytes));
  env->SetLongField(stats, rxBytesField, static_cast<jlong>(res->ForwardBytes));
  env->SetLongField(stats, txPacketsField, static_cast<jlong>(res->BackwardPackets));
  env->SetLongField(stats, rxPacketsField, static_cast<jlong>(res->ForwardPackets));
  env->SetLongField(stats, rttMsField, static_cast<jlong>(res->RttMs));
  return JNI_TRUE;
}

JNIEXPORT void JNICALL Java_info_kghost_android_1hole_vpn_dataplane_JniTunnelDataPlaneNative_nativeStop(
    JNIEnv* env, jclass clazz, jlong sessionHandle) {
  auto session = GetSession(sessionHandle);
  if (session) {
    session->Stop();
  }
}

JNIEXPORT void JNICALL Java_info_kghost_android_1hole_vpn_dataplane_JniTunnelDataPlaneNative_nativeDestroy(
    JNIEnv* env, jclass clazz, jlong sessionHandle) {
  auto* session = reinterpret_cast<gh::JniSession*>(sessionHandle);
  if (session) {
    session->Stop();
    delete session;
  }
}

} // extern "C"
