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
#include <unordered_map>
#include <utility>
#include <vector>

#include <boost/asio.hpp>
#include <boost/log/core.hpp>
#include <boost/log/expressions.hpp>
#include <boost/log/sinks/basic_sink_backend.hpp>
#include <boost/log/sinks/sync_frontend.hpp>
#include <boost/log/trivial.hpp>
#include <boost/make_shared.hpp>

#include "Asio.hpp"
#include "ConnectionTracker.hpp"
#include "Coroutine.hpp"
#include "ExternalQueue.hpp"
#include "InterfaceCommonTypes.hpp"
#include "MoveOnlyFunction.hpp"
#include "TunnelDataPlane.hpp"
#include "Utils.hpp"
#include "Utils/Overload.hpp"

static JavaVM* g_JavaVM = nullptr;
static jclass g_CallbacksClass = nullptr;
static jmethodID g_MidFindTunnelForFlow = nullptr;
static jmethodID g_MidOnTunnelStateChanged = nullptr;
static jmethodID g_MidOnVpnStateChanged = nullptr;

namespace gh {

class JniSession : public Interface::DataPlaneCallbacks {
public:
  using Task = Omni::Fiber::move_only_function<Omni::Fiber::Coroutine<void>(TunnelDataPlane&, bool&)>;

  explicit JniSession(JNIEnv* env, jobject callbacks, jobject connectivityManager);
  ~JniSession() override;

  JniSession(const JniSession&) = delete;
  auto operator=(const JniSession&) -> JniSession& = delete;
  JniSession(JniSession&&) = delete;
  auto operator=(JniSession&&) -> JniSession& = delete;

  static auto GetEnv() -> JNIEnv* {
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
  auto AddEndpoint(const UdpDynMux::PskType& psk, const std::string& address) -> jlong;
  void RemoveEndpoint(jlong handle);
  void StartEndpoint(jlong handle);
  void StopEndpoint(jlong handle);

  auto GetCallbacks() const -> jobject { return _Callbacks; }

  auto ProtectSocket(int fd) -> bool;
  void OnVpnStateChanged(Interface::TunnelState state, const std::string& message) override;
  void OnEndpointStateChanged(Interface::VpnEndpoint endpoint, Interface::TunnelState state,
                              const std::string& error) override;
  auto GetTrafficStats(jlong endpointHandle) -> std::optional<Interface::VpnTrafficStats>;

  auto FindSession(const VpnClientMultiChannelSession* ptr) const -> std::weak_ptr<VpnClientMultiChannelSession> {
    auto iter = _EndpointMap.find(ptr);
    return iter != _EndpointMap.end() ? iter->second : std::weak_ptr<VpnClientMultiChannelSession>{};
  }

  template <typename Func> void PostTask(Func&& func) {
    _TaskQueue.Push(
        [func = std::forward<Func>(func)](TunnelDataPlane& dataplane, bool& stop) -> Omni::Fiber::Coroutine<void> {
          co_await func(dataplane, stop);
        });
  }

private:
  jobject _Callbacks = nullptr;
  jobject _ConnectivityManager = nullptr;
  boost::asio::io_context _IoContext;
  std::thread _Thread;

  Omni::Fiber::ExternalQueue<Task> _TaskQueue;

  std::unordered_map<const VpnClientMultiChannelSession*, std::weak_ptr<VpnClientMultiChannelSession>> _EndpointMap;

  std::atomic<bool> _Stopped{false};
};

class PolicyResolverCallback : public TunnelDataPlanePolicyResolverCallback {
public:
  explicit PolicyResolverCallback(JniSession& session) : _Session(session) {}
  ~PolicyResolverCallback() override = default;

  PolicyResolverCallback(const PolicyResolverCallback&) = default;
  PolicyResolverCallback(PolicyResolverCallback&&) = delete;
  auto operator=(const PolicyResolverCallback&) -> PolicyResolverCallback& = delete;
  auto operator=(PolicyResolverCallback&&) -> PolicyResolverCallback& = delete;

  auto ResolvePolicy(const ConnectionTracker::ConnectionKey& key) -> Interface::PolicyRule::RoutingAction override;

private:
  [[nodiscard]] auto FindTunnel(int protocol, const boost::asio::ip::address& localAddr, uint16_t localPort,
                                const boost::asio::ip::address& remoteAddr, uint16_t remotePort) const
      -> Interface::PolicyRule::RoutingAction;

  JniSession& _Session;
};

// PolicyResolverCallback Implementation
auto PolicyResolverCallback::FindTunnel(int protocol, const boost::asio::ip::address& localAddr, uint16_t localPort,
                                        const boost::asio::ip::address& remoteAddr, uint16_t remotePort) const
    -> Interface::PolicyRule::RoutingAction {

  JNIEnv* env = JniSession::GetEnv();
  jbyteArray localBytes = env->NewByteArray(16);
  if ((localBytes == nullptr) || (env->ExceptionCheck() != 0U)) {
    BOOST_LOG_TRIVIAL(warning) << "JNI exception or allocation failure for localBytes in FindTunnel";
    if (env->ExceptionCheck() != 0U) {
      env->ExceptionClear();
    }
    return Interface::PolicyRule::DiscardRoute{};
  }
  auto v6Local = MapToV6(localAddr).to_bytes();
  env->SetByteArrayRegion(localBytes, 0, 16, reinterpret_cast<const jbyte*>(v6Local.data()));

  jbyteArray remoteBytes = env->NewByteArray(16);
  if ((remoteBytes == nullptr) || (env->ExceptionCheck() != 0U)) {
    BOOST_LOG_TRIVIAL(warning) << "JNI exception or allocation failure for remoteBytes in FindTunnel";
    if (env->ExceptionCheck() != 0U) {
      env->ExceptionClear();
    }
    env->DeleteLocalRef(localBytes);
    return Interface::PolicyRule::DiscardRoute{};
  }
  auto v6Remote = MapToV6(remoteAddr).to_bytes();
  env->SetByteArrayRegion(remoteBytes, 0, 16, reinterpret_cast<const jbyte*>(v6Remote.data()));

  jlong endpointHandle = env->CallLongMethod(_Session.GetCallbacks(), g_MidFindTunnelForFlow, protocol, localBytes,
                                             static_cast<jint>(localPort), remoteBytes, static_cast<jint>(remotePort));
  if (env->ExceptionCheck() != 0U) {
    BOOST_LOG_TRIVIAL(warning) << "JNI exception in FindTunnel (findTunnelForFlow) for protocol " << protocol;
    env->ExceptionDescribe();
    env->ExceptionClear();
    endpointHandle = 0;
  }

  env->DeleteLocalRef(localBytes);
  env->DeleteLocalRef(remoteBytes);

  if (endpointHandle != 0) {
    auto session = _Session.FindSession(reinterpret_cast<const VpnClientMultiChannelSession*>(endpointHandle));
    if (auto sharedSession = session.lock()) {
      return Interface::PolicyRule::EndpointRoute{session};
    }
  }
  return Interface::PolicyRule::DiscardRoute{};
}

auto PolicyResolverCallback::ResolvePolicy(const ConnectionTracker::ConnectionKey& key)
    -> Interface::PolicyRule::RoutingAction {
  return std::visit(
      Overload{[this](const ConnectionTracker::Ip4TcpKey& key) -> Interface::PolicyRule::RoutingAction {
                 return FindTunnel(6, key.LocalAddress, key.LocalPort, key.RemoteAddress, key.RemotePort);
               },
               [this](const ConnectionTracker::Ip6TcpKey& key) -> Interface::PolicyRule::RoutingAction {
                 return FindTunnel(6, key.LocalAddress, key.LocalPort, key.RemoteAddress, key.RemotePort);
               },
               [this](const ConnectionTracker::Ip4UdpKey& key) -> Interface::PolicyRule::RoutingAction {
                 return FindTunnel(17, key.LocalAddress, key.LocalPort, key.RemoteAddress, key.RemotePort);
               },
               [this](const ConnectionTracker::Ip6UdpKey& key) -> Interface::PolicyRule::RoutingAction {
                 return FindTunnel(17, key.LocalAddress, key.LocalPort, key.RemoteAddress, key.RemotePort);
               },
               [this](const ConnectionTracker::IcmpKey& key) -> Interface::PolicyRule::RoutingAction {
                 return FindTunnel(1, key.LocalAddress, key.Id, key.RemoteAddress, 0);
               },
               [this](const ConnectionTracker::Icmp6Key& key) -> Interface::PolicyRule::RoutingAction {
                 return FindTunnel(58, key.LocalAddress, key.Id, key.RemoteAddress, 0);
               }},
      key);
}

// JniSession Implementation

JniSession::JniSession(JNIEnv* env, jobject callbacks, jobject connectivityManager)
    : _Callbacks(env->NewGlobalRef(callbacks)), _ConnectivityManager(env->NewGlobalRef(connectivityManager)),
      _TaskQueue(_IoContext.get_executor()) {
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

    manager.SpawnRoot("root", [this]() -> Omni::Fiber::Coroutine<void> {
      auto policyResolver = std::make_unique<PolicyResolverCallback>(*this);
      TunnelDataPlane dataplane(_IoContext.get_executor(), *policyResolver, *this);
      bool stop = false;
      while (!stop) {
        co_await _TaskQueue;
        while (!_TaskQueue.IsEmpty()) {
          auto task = _TaskQueue.PopFront();
          co_await task(dataplane, stop);
        }
      }
      co_return;
    });

    _IoContext.run();
  });
}

JniSession::~JniSession() {
  Stop();
  JNIEnv* env = GetEnv();
  env->DeleteGlobalRef(_Callbacks);
  env->DeleteGlobalRef(_ConnectivityManager);
}

void JniSession::Start(int tunFd, int mtu, std::vector<char> encryptionKey) {
  PostTask([this, tunFd, mtu, encryptionKey = std::move(encryptionKey)](TunnelDataPlane& dataplane,
                                                                        bool& stop) -> Omni::Fiber::Coroutine<void> {
    auto err = co_await dataplane.Start(tunFd, mtu, encryptionKey);
    if (err) {
      BOOST_LOG_TRIVIAL(error) << "Failed to start TunnelDataPlane: " << err.message();
      OnVpnStateChanged(Interface::TunnelState::Failed, err.message());
    }
    co_return;
  });
}

void JniSession::MigrateTun(int tunFd) {
  PostTask([tunFd](TunnelDataPlane& dataplane, bool& stop) -> Omni::Fiber::Coroutine<void> {
    co_await dataplane.MigrateTun(tunFd);
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

  PostTask([&promise](TunnelDataPlane& dataplane, bool& stop) -> Omni::Fiber::Coroutine<void> {
    co_await dataplane.Stop();
    stop = true;
    promise.set_value();
    co_return;
  });

  future.get();

  if (_Thread.joinable()) {
    _Thread.join();
  }
}

auto JniSession::AddEndpoint(const UdpDynMux::PskType& psk, const std::string& address) -> jlong {
  std::promise<jlong> promise;
  auto future = promise.get_future();

  PostTask([this, &promise, psk, address](TunnelDataPlane& dataplane, bool& stop) -> Omni::Fiber::Coroutine<void> {
    auto weak = dataplane.AddEndpoint(psk, address);
    auto session = weak.lock();
    assert(session);
    _EndpointMap[session.get()] = weak;
    co_await dataplane.StartEndpoint(weak);
    promise.set_value(reinterpret_cast<jlong>(session.get())); // NOLINT(cppcoreguidelines-pro-type-reinterpret-cast)
    co_return;
  });

  return future.get();
}

void JniSession::RemoveEndpoint(jlong handle) {
  std::promise<void> promise;
  auto future = promise.get_future();

  PostTask([this, &promise, handle](TunnelDataPlane& dataplane, bool& stop) -> Omni::Fiber::Coroutine<void> {
    // NOLINTNEXTLINE(cppcoreguidelines-pro-type-reinterpret-cast, performance-no-int-to-ptr)
    auto* ptr = reinterpret_cast<VpnClientMultiChannelSession*>(handle);
    auto iterator = _EndpointMap.find(ptr);
    if (iterator != _EndpointMap.end()) {
      auto weak = iterator->second;
      co_await dataplane.StopEndpoint(weak);
      dataplane.RemoveEndpoint(weak);
      _EndpointMap.erase(iterator);
    }
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

void JniSession::OnVpnStateChanged(Interface::TunnelState state, const std::string& message) {
  if (_Callbacks == nullptr) {
    return;
  }
  JNIEnv* env = GetEnv();
  jstring msgStr = !message.empty() ? env->NewStringUTF(message.c_str()) : nullptr;
  if (env->ExceptionCheck() != 0U) {
    BOOST_LOG_TRIVIAL(warning) << "JNI exception in OnVpnStateChanged (NewStringUTF)";
    env->ExceptionClear();
  }
  env->CallVoidMethod(_Callbacks, g_MidOnVpnStateChanged, static_cast<jint>(state), msgStr);
  if (env->ExceptionCheck() != 0U) {
    BOOST_LOG_TRIVIAL(warning) << "JNI exception in OnVpnStateChanged (onVpnStateChanged)";
    env->ExceptionDescribe();
    env->ExceptionClear();
  }
  if (msgStr != nullptr) {
    env->DeleteLocalRef(msgStr);
  }
}

void JniSession::OnEndpointStateChanged(Interface::VpnEndpoint endpoint, Interface::TunnelState state,
                                        const std::string& error) {
  if (!_Callbacks) {
    return;
  }
  JNIEnv* env = GetEnv();
  jstring errStr = !error.empty() ? env->NewStringUTF(error.c_str()) : nullptr;
  if (env->ExceptionCheck() != 0U) {
    BOOST_LOG_TRIVIAL(warning) << "JNI exception in OnEndpointStateChanged (NewStringUTF)";
    env->ExceptionClear();
  }
  env->CallVoidMethod(_Callbacks, g_MidOnTunnelStateChanged, reinterpret_cast<jlong>(endpoint.lock().get()),
                      static_cast<jint>(std::to_underlying(state)), errStr);
  if (env->ExceptionCheck() != 0U) {
    BOOST_LOG_TRIVIAL(warning) << "JNI exception in OnEndpointStateChanged (onTunnelStateChanged)";
    env->ExceptionDescribe();
    env->ExceptionClear();
  }
  if (errStr != nullptr) {
    env->DeleteLocalRef(errStr);
  }
}

auto JniSession::GetTrafficStats(jlong endpointHandle) -> std::optional<Interface::VpnTrafficStats> {
  std::promise<std::optional<Interface::VpnTrafficStats>> promise;
  auto future = promise.get_future();

  PostTask([this, &promise, endpointHandle](TunnelDataPlane& dataplane, bool& stop) -> Omni::Fiber::Coroutine<void> {
    auto* ptr = reinterpret_cast<VpnClientMultiChannelSession*>(endpointHandle);
    auto session = FindSession(ptr);
    promise.set_value(TunnelDataPlane::GetTrafficStats(session));
    co_return;
  });
  return future.get();
}

} // namespace gh

namespace {

auto GetSession(jlong handle) -> gh::JniSession* { return reinterpret_cast<gh::JniSession*>(handle); }

class AndroidLogBackend
    : public boost::log::sinks::basic_formatted_sink_backend<char, boost::log::sinks::synchronized_feeding> {
public:
  static void consume(const boost::log::record_view& rec, const string_type& formattedMessage) {
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

JNIEXPORT auto JNICALL JNI_OnLoad(JavaVM* jvm, void* /*reserved*/) -> jint {
  g_JavaVM = jvm;
  JNIEnv* env = nullptr;
  if (jvm->GetEnv((void**)&env, JNI_VERSION_1_6) != JNI_OK) {
    return JNI_ERR;
  }

  SetupLogging();

  ares_library_init_jvm(jvm);

  jclass localCallbacksClass = env->FindClass("info/kghost/android_hole/vpn/dataplane/TunnelDataPlaneCallbacks");
  if (localCallbacksClass == nullptr) {
    return JNI_ERR;
  }
  g_CallbacksClass = (jclass)env->NewGlobalRef(localCallbacksClass);

  g_MidFindTunnelForFlow = env->GetMethodID(g_CallbacksClass, "findTunnelForFlow", "(I[BI[BI)J");
  g_MidOnTunnelStateChanged = env->GetMethodID(g_CallbacksClass, "onTunnelStateChanged", "(JILjava/lang/String;)V");
  g_MidOnVpnStateChanged = env->GetMethodID(g_CallbacksClass, "onVpnStateChanged", "(ILjava/lang/String;)V");

  if ((g_MidFindTunnelForFlow == nullptr) || (g_MidOnTunnelStateChanged == nullptr) ||
      (g_MidOnVpnStateChanged == nullptr)) {
    return JNI_ERR;
  }

  return JNI_VERSION_1_6;
}

JNIEXPORT void JNICALL JNI_OnUnload(JavaVM* jvm, void* /*reserved*/) {
  JNIEnv* env = nullptr;
  if (jvm->GetEnv((void**)&env, JNI_VERSION_1_6) == JNI_OK) {
    if (g_CallbacksClass != nullptr) {
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

  env->SetLongField(stats, txBytesField, static_cast<jlong>(res->ForwardBytes));
  env->SetLongField(stats, rxBytesField, static_cast<jlong>(res->BackwardBytes));
  env->SetLongField(stats, txPacketsField, static_cast<jlong>(res->ForwardPackets));
  env->SetLongField(stats, rxPacketsField, static_cast<jlong>(res->BackwardPackets));
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
