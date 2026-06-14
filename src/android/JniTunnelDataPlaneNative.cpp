#include "info_kghost_android_hole_vpn_dataplane_JniTunnelDataPlaneNative.h"

#include <chrono>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include <boost/asio.hpp>
#include <boost/log/trivial.hpp>
#include <jni.h>

#include "Asio.hpp"
#include "Cancel.hpp"
#include "Coroutine.hpp"
#include "EndpointTun.hpp"
#include "EndpointUdpDynMux.hpp"
#include "ResolverHelper.hpp"
#include "ResolverStaticEndpoint.hpp"
#include "Utils.hpp"
#include "VpnClientMultiChannel.hpp"

static JavaVM* g_JavaVM = nullptr;
static jclass g_CallbacksClass = nullptr;
static jmethodID g_MidProtectSocket = nullptr;
static jmethodID g_MidFindTunnelForFlow = nullptr;
static jmethodID g_MidOnStateChanged = nullptr;
static jmethodID g_MidOnTrafficStats = nullptr;

static jclass g_StateClass = nullptr;
static jobject g_StateConnecting = nullptr;
static jobject g_StateConnected = nullptr;
static jobject g_StateDisconnected = nullptr;
static jobject g_StateFailed = nullptr;

namespace gh {

class JniSession;

class JniSelector : public ConnectionTracker::Selector {
public:
  explicit JniSelector(JniSession& session) : _Session(session) {}
  ~JniSelector() override = default;

  std::optional<std::reference_wrapper<ConnectionMark>> operator()(const ConnectionTracker::Ip4TcpKey& key) const override;
  std::optional<std::reference_wrapper<ConnectionMark>> operator()(const ConnectionTracker::Ip6TcpKey& key) const override;
  std::optional<std::reference_wrapper<ConnectionMark>> operator()(const ConnectionTracker::Ip4UdpKey& key) const override;
  std::optional<std::reference_wrapper<ConnectionMark>> operator()(const ConnectionTracker::Ip6UdpKey& key) const override;
  std::optional<std::reference_wrapper<ConnectionMark>> operator()(const ConnectionTracker::IcmpKey& key) const override;
  std::optional<std::reference_wrapper<ConnectionMark>> operator()(const ConnectionTracker::Icmp6Key& key) const override;

private:
  std::optional<std::reference_wrapper<ConnectionMark>> FindTunnel(
      int protocol, const boost::asio::ip::address& localAddr, uint16_t localPort,
      const boost::asio::ip::address& remoteAddr, uint16_t remotePort) const;

  JniSession& _Session;
};

class JniSession {
public:
  explicit JniSession(jobject callbacks) {
    JNIEnv* env = GetEnv();
    _Callbacks = env->NewGlobalRef(callbacks);
  }

  ~JniSession() {
    JNIEnv* env = GetEnv();
    if (_Callbacks && env) {
      env->DeleteGlobalRef(_Callbacks);
    }
  }

  static JNIEnv* GetEnv() {
    JNIEnv* env = nullptr;
    jint res = g_JavaVM->GetEnv((void**)&env, JNI_VERSION_1_6);
    if (res == JNI_EDETACHED) {
      g_JavaVM->AttachCurrentThreadAsDaemon(&env, nullptr);
    }
    return env;
  }

  void Start(int tunFd, int mtu) {
    _Work = std::make_unique<boost::asio::io_context::work>(_IoContext);
    _Thread = std::thread([this, tunFd, mtu]() {
      JNIEnv* env = GetEnv();
      JavaVMAttachArgs args;
      args.version = JNI_VERSION_1_6;
      args.name = (char*)"great_hole_worker";
      args.group = nullptr;
      g_JavaVM->AttachCurrentThreadAsDaemon(&env, &args);

      gh::SocketProtector = [this](int fd) -> bool {
        JNIEnv* localEnv = GetEnv();
        if (!localEnv || !_Callbacks) {
          return false;
        }
        return localEnv->CallBooleanMethod(_Callbacks, g_MidProtectSocket, fd);
      };

      auto ioExecutor = _IoContext.get_executor();
      Omni::Fiber::AsioExecutor executor(ioExecutor);
      Omni::Fiber::Manager manager(executor);

      _Tun = std::make_shared<Tun>(ioExecutor, tunFd);
      _UdpDynMux = std::make_shared<UdpDynMux>(ioExecutor);
      _Selector = std::make_unique<JniSelector>(*this);
      _Client = std::make_shared<VpnClientMultiChannel>(ioExecutor, _Tun, _UdpDynMux, *_Selector);

      _RootFiber = manager.SpawnRoot("root", [this]() -> Omni::Fiber::Coroutine<void> {
        UpdateState(g_StateConnecting, "VPN starting");

        auto ec = co_await _Tun->Start();
        if (ec) {
          BOOST_LOG_TRIVIAL(error) << "Failed to start Tun: " << ec.message();
          UpdateState(g_StateFailed, ec.message().c_str());
          co_return;
        }

        ec = co_await _UdpDynMux->Start();
        if (ec) {
          BOOST_LOG_TRIVIAL(error) << "Failed to start UdpDynMux: " << ec.message();
          UpdateState(g_StateFailed, ec.message().c_str());
          co_return;
        }

        ec = co_await _Client->Start();
        if (ec) {
          BOOST_LOG_TRIVIAL(error) << "Failed to start VpnClientMultiChannel: " << ec.message();
          UpdateState(g_StateFailed, ec.message().c_str());
          co_return;
        }

        UpdateState(g_StateConnected, "VPN tunnel established");

        _StatsTimer = std::make_shared<boost::asio::steady_timer>(_IoContext);
        StartStatsLoop();

        co_await _StopEvent.GetConsumer();

        if (_StatsTimer) {
          _StatsTimer->cancel();
          _StatsTimer.reset();
        }

        co_await _Client->Stop();
        co_await _Client->WaitService();

        co_await _UdpDynMux->Stop();
        co_await _UdpDynMux->WaitService();

        co_await _Tun->Stop();
        co_await _Tun->WaitService();

        gh::SocketProtector = nullptr;
        UpdateState(g_StateDisconnected, "VPN tunnel stopped");
        co_return;
      });

      _IoContext.run();
    });
  }

  void Stop() {
    _IoContext.post([this]() {
      if (_RootFiber) {
        _RootFiber->Spawn("stop", [this]() -> Omni::Fiber::Coroutine<void> {
          co_await _StopEvent.GetProducer().Put(true);
          co_return;
        });
      }
      _Work.reset();
    });
    if (_Thread.joinable()) {
      _Thread.join();
    }
  }

  void UpdateState(jobject state, const char* message) {
    if (!state || !_Callbacks) {
      return;
    }
    JNIEnv* env = GetEnv();
    jstring msgStr = message ? env->NewStringUTF(message) : nullptr;
    env->CallVoidMethod(_Callbacks, g_MidOnStateChanged, state, msgStr);
    if (msgStr) {
      env->DeleteLocalRef(msgStr);
    }
  }

  jlong AddEndpoint(const std::string& displayName, const std::string& host, int port) {
    std::lock_guard<std::mutex> lock(_Mutex);
    jlong handle = ++_NextHandle;
    auto psk = ParsePsk(displayName);

    _IoContext.post([this, handle, psk, host, port]() {
      if (_RootFiber) {
        _RootFiber->Spawn("add_endpoint", [this, handle, psk, host, port]() -> Omni::Fiber::Coroutine<void> {
          std::shared_ptr<ResolverEndpoint> resolver;
          if (_UdpDynMux) {
            std::string addrStr = host + ":" + std::to_string(port);
            resolver = FindResolverEndpoint(addrStr, *_UdpDynMux);
          }
          if (_Client) {
            auto refSession = co_await _Client->RegisterChannel(psk, resolver);
            std::lock_guard<std::mutex> lock2(_Mutex);
            _Endpoints.emplace(handle, EndpointEntry{psk, refSession});
          }
          co_return;
        });
      }
    });

    return handle;
  }

  void RemoveEndpoint(jlong handle) {
    std::lock_guard<std::mutex> lock(_Mutex);
    auto it = _Endpoints.find(handle);
    if (it != _Endpoints.end()) {
      auto psk = it->second.Psk;
      _Endpoints.erase(it);
      _IoContext.post([this, psk]() {
        if (_RootFiber) {
          _RootFiber->Spawn("remove_endpoint", [this, psk]() -> Omni::Fiber::Coroutine<void> {
            if (_Client) {
              co_await _Client->UnregisterChannel(psk);
            }
            co_return;
          });
        }
      });
    }
  }

  void StartEndpoint(jlong handle) {
    // Negotiation is automatically started on register/setup
  }

  void StopEndpoint(jlong handle) {
    // Handled via remove or stop
  }

  jobject GetCallbacks() const { return _Callbacks; }

  std::optional<std::reference_wrapper<ConnectionMark>> FindSessionByHandle(jlong handle) {
    std::lock_guard<std::mutex> lock(_Mutex);
    auto it = _Endpoints.find(handle);
    if (it != _Endpoints.end()) {
      return it->second.SessionRef;
    }
    return std::nullopt;
  }

private:
  UdpDynMux::PskType ParsePsk(const std::string& displayName) {
    UdpDynMux::PskType psk{};
    if (displayName.length() == 32) {
      bool valid = true;
      for (size_t i = 0; i < 16; ++i) {
        std::string byteString = displayName.substr(i * 2, 2);
        char* end;
        long val = std::strtol(byteString.c_str(), &end, 16);
        if (*end != '\0') {
          valid = false;
          break;
        }
        psk[i] = static_cast<uint8_t>(val);
      }
      if (valid) {
        return psk;
      }
    }
    uint64_t hash1 = 14695981039346656037ULL;
    uint64_t hash2 = 14695981039346656037ULL;
    for (char c : displayName) {
      hash1 = (hash1 ^ c) * 1099511628211ULL;
    }
    for (size_t i = 0; i < displayName.length(); ++i) {
      hash2 = (hash2 ^ displayName[displayName.length() - 1 - i]) * 1099511628211ULL;
    }
    std::memcpy(psk.data(), &hash1, 8);
    std::memcpy(psk.data() + 8, &hash2, 8);
    return psk;
  }

  void StartStatsLoop() {
    if (!_StatsTimer) {
      return;
    }
    _StatsTimer->expires_after(std::chrono::seconds(2));
    _StatsTimer->async_wait([this](const boost::system::error_code& ec) {
      if (ec) {
        return;
      }
      ReportStats();
      StartStatsLoop();
    });
  }

  void ReportStats() {
    if (!_Client) {
      return;
    }
    JNIEnv* env = GetEnv();
    if (!env || !_Callbacks) {
      return;
    }

    std::lock_guard<std::mutex> lock(_Mutex);
    for (auto& [handle, entry] : _Endpoints) {
      auto [tx, rx] = _Client->GetStats(entry.Psk);
      if (tx > 0 || rx > 0) {
        env->CallVoidMethod(_Callbacks, g_MidOnTrafficStats, handle, static_cast<jlong>(tx), static_cast<jlong>(rx));
      }
    }
  }

  struct EndpointEntry {
    UdpDynMux::PskType Psk;
    std::reference_wrapper<VpnClientMultiChannel::Session> SessionRef;
  };

  jobject _Callbacks = nullptr;
  boost::asio::io_context _IoContext;
  std::unique_ptr<boost::asio::io_context::work> _Work;
  std::thread _Thread;

  std::shared_ptr<Tun> _Tun;
  std::shared_ptr<UdpDynMux> _UdpDynMux;
  std::unique_ptr<JniSelector> _Selector;
  std::shared_ptr<VpnClientMultiChannel> _Client;
  std::shared_ptr<Omni::Fiber::Fiber> _RootFiber;
  Omni::Fiber::Pipe<bool> _StopEvent;
  std::shared_ptr<boost::asio::steady_timer> _StatsTimer;

  std::mutex _Mutex;
  jlong _NextHandle = 0;
  std::map<jlong, EndpointEntry> _Endpoints;
};

std::optional<std::reference_wrapper<ConnectionMark>> JniSelector::FindTunnel(
    int protocol, const boost::asio::ip::address& localAddr, uint16_t localPort,
    const boost::asio::ip::address& remoteAddr, uint16_t remotePort) const {

  JNIEnv* env = JniSession::GetEnv();
  if (!env || !_Session.GetCallbacks()) {
    return std::nullopt;
  }

  jbyteArray localBytes = env->NewByteArray(16);
  auto v6Local = MapToV6(localAddr).to_bytes();
  env->SetByteArrayRegion(localBytes, 0, 16, reinterpret_cast<const jbyte*>(v6Local.data()));

  jbyteArray remoteBytes = env->NewByteArray(16);
  auto v6Remote = MapToV6(remoteAddr).to_bytes();
  env->SetByteArrayRegion(remoteBytes, 0, 16, reinterpret_cast<const jbyte*>(v6Remote.data()));

  jlong endpointHandle = env->CallLongMethod(_Session.GetCallbacks(), g_MidFindTunnelForFlow,
                                             protocol, localBytes, static_cast<jint>(localPort),
                                             remoteBytes, static_cast<jint>(remotePort));

  env->DeleteLocalRef(localBytes);
  env->DeleteLocalRef(remoteBytes);

  if (endpointHandle > 0) {
    return _Session.FindSessionByHandle(endpointHandle);
  }
  return std::nullopt;
}

std::optional<std::reference_wrapper<ConnectionMark>> JniSelector::operator()(const ConnectionTracker::Ip4TcpKey& key) const {
  return FindTunnel(6, key.LocalAddress, key.LocalPort, key.RemoteAddress, key.RemotePort);
}
std::optional<std::reference_wrapper<ConnectionMark>> JniSelector::operator()(const ConnectionTracker::Ip6TcpKey& key) const {
  return FindTunnel(6, key.LocalAddress, key.LocalPort, key.RemoteAddress, key.RemotePort);
}
std::optional<std::reference_wrapper<ConnectionMark>> JniSelector::operator()(const ConnectionTracker::Ip4UdpKey& key) const {
  return FindTunnel(17, key.LocalAddress, key.LocalPort, key.RemoteAddress, key.RemotePort);
}
std::optional<std::reference_wrapper<ConnectionMark>> JniSelector::operator()(const ConnectionTracker::Ip6UdpKey& key) const {
  return FindTunnel(17, key.LocalAddress, key.LocalPort, key.RemoteAddress, key.RemotePort);
}
std::optional<std::reference_wrapper<ConnectionMark>> JniSelector::operator()(const ConnectionTracker::IcmpKey& key) const {
  return FindTunnel(1, key.LocalAddress, key.Id, key.RemoteAddress, 0);
}
std::optional<std::reference_wrapper<ConnectionMark>> JniSelector::operator()(const ConnectionTracker::Icmp6Key& key) const {
  return FindTunnel(58, key.LocalAddress, key.Id, key.RemoteAddress, 0);
}

} // namespace gh

static std::mutex g_SessionsMutex;
static std::map<jlong, std::shared_ptr<gh::JniSession>> g_Sessions;
static jlong g_NextSessionId = 1;

static std::shared_ptr<gh::JniSession> GetSession(jlong handle) {
  std::lock_guard<std::mutex> lock(g_SessionsMutex);
  auto it = g_Sessions.find(handle);
  if (it != g_Sessions.end()) {
    return it->second;
  }
  return nullptr;
}

JNIEXPORT jint JNICALL JNI_OnLoad(JavaVM* vm, void* reserved) {
  g_JavaVM = vm;
  JNIEnv* env = nullptr;
  if (vm->GetEnv((void**)&env, JNI_VERSION_1_6) != JNI_OK) {
    return JNI_ERR;
  }

  jclass localCallbacksClass = env->FindClass("info/kghost/android_hole/vpn/dataplane/TunnelDataPlaneCallbacks");
  if (!localCallbacksClass) {
    return JNI_ERR;
  }
  g_CallbacksClass = (jclass)env->NewGlobalRef(localCallbacksClass);

  g_MidProtectSocket = env->GetMethodID(g_CallbacksClass, "protectSocket", "(I)Z");
  g_MidFindTunnelForFlow = env->GetMethodID(g_CallbacksClass, "findTunnelForFlow", "(I[BI[BI)J");
  g_MidOnStateChanged = env->GetMethodID(g_CallbacksClass, "onStateChanged", "(Linfo/kghost/android_hole/vpn/dataplane/NativeTunnelState;Ljava/lang/String;)V");
  g_MidOnTrafficStats = env->GetMethodID(g_CallbacksClass, "onTrafficStats", "(JJJ)V");

  if (!g_MidProtectSocket || !g_MidFindTunnelForFlow || !g_MidOnStateChanged || !g_MidOnTrafficStats) {
    return JNI_ERR;
  }

  jclass localStateClass = env->FindClass("info/kghost/android_hole/vpn/dataplane/NativeTunnelState");
  if (localStateClass) {
    g_StateClass = (jclass)env->NewGlobalRef(localStateClass);

    auto getEnum = [&](const char* name) -> jobject {
      jfieldID fid = env->GetStaticFieldID(g_StateClass, name, "Linfo/kghost/android_hole/vpn/dataplane/NativeTunnelState;");
      if (env->ExceptionCheck()) {
        env->ExceptionClear();
        return nullptr;
      }
      jobject val = env->GetStaticObjectField(g_StateClass, fid);
      return val ? env->NewGlobalRef(val) : nullptr;
    };

    g_StateConnecting = getEnum("CONNECTING");
    g_StateConnected = getEnum("CONNECTED");
    g_StateDisconnected = getEnum("DISCONNECTED");
    g_StateFailed = getEnum("FAILED");
  }

  return JNI_VERSION_1_6;
}

JNIEXPORT void JNICALL JNI_OnUnload(JavaVM* vm, void* reserved) {
  JNIEnv* env = nullptr;
  if (vm->GetEnv((void**)&env, JNI_VERSION_1_6) == JNI_OK) {
    if (g_CallbacksClass) {
      env->DeleteGlobalRef(g_CallbacksClass);
    }
    if (g_StateClass) {
      env->DeleteGlobalRef(g_StateClass);
    }
    if (g_StateConnecting) {
      env->DeleteGlobalRef(g_StateConnecting);
    }
    if (g_StateConnected) {
      env->DeleteGlobalRef(g_StateConnected);
    }
    if (g_StateDisconnected) {
      env->DeleteGlobalRef(g_StateDisconnected);
    }
    if (g_StateFailed) {
      env->DeleteGlobalRef(g_StateFailed);
    }
  }
}

extern "C" {

JNIEXPORT jlong JNICALL Java_info_kghost_android_1hole_vpn_dataplane_JniTunnelDataPlaneNative_nativeCreate(
    JNIEnv* env, jclass clazz, jobject callbacks) {
  auto session = std::shared_ptr<gh::JniSession>(new gh::JniSession(callbacks));
  std::lock_guard<std::mutex> lock(g_SessionsMutex);
  jlong handle = g_NextSessionId++;
  g_Sessions[handle] = session;
  return handle;
}

JNIEXPORT void JNICALL Java_info_kghost_android_1hole_vpn_dataplane_JniTunnelDataPlaneNative_nativeStart(
    JNIEnv* env, jclass clazz, jlong sessionHandle, jint tunFd, jint mtu) {
  auto session = GetSession(sessionHandle);
  if (session) {
    session->Start(tunFd, mtu);
  }
}

JNIEXPORT jlong JNICALL Java_info_kghost_android_1hole_vpn_dataplane_JniTunnelDataPlaneNative_nativeAddEndpoint(
    JNIEnv* env, jclass clazz, jlong sessionHandle, jstring displayName, jstring host, jint port) {
  auto session = GetSession(sessionHandle);
  if (session) {
    const char* dNameChars = env->GetStringUTFChars(displayName, nullptr);
    const char* hostChars = env->GetStringUTFChars(host, nullptr);
    std::string displayNameStr(dNameChars);
    std::string hostStr(hostChars);
    env->ReleaseStringUTFChars(displayName, dNameChars);
    env->ReleaseStringUTFChars(host, hostChars);

    return session->AddEndpoint(displayNameStr, hostStr, port);
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

JNIEXPORT void JNICALL Java_info_kghost_android_1hole_vpn_dataplane_JniTunnelDataPlaneNative_nativeStop(
    JNIEnv* env, jclass clazz, jlong sessionHandle) {
  auto session = GetSession(sessionHandle);
  if (session) {
    session->Stop();
  }
}

JNIEXPORT void JNICALL Java_info_kghost_android_1hole_vpn_dataplane_JniTunnelDataPlaneNative_nativeDestroy(
    JNIEnv* env, jclass clazz, jlong sessionHandle) {
  std::shared_ptr<gh::JniSession> session;
  {
    std::lock_guard<std::mutex> lock(g_SessionsMutex);
    auto it = g_Sessions.find(sessionHandle);
    if (it != g_Sessions.end()) {
      session = it->second;
      g_Sessions.erase(it);
    }
  }
  if (session) {
    session->Stop();
  }
}

} // extern "C"
