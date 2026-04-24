#include <jni.h>

#include <atomic>
#include <mutex>
#include <string>
#include <vector>

#include "CarSerialService.h"
#include "Log.h"

using carserial::CarSerialService;

static JavaVM* g_vm = nullptr;
static std::mutex g_mu;
static jobject g_sink = nullptr;
static jmethodID g_on_event = nullptr;

static JNIEnv* getEnv(bool* needsDetach) {
  *needsDetach = false;
  if (!g_vm) return nullptr;
  JNIEnv* env = nullptr;
  jint res = g_vm->GetEnv(reinterpret_cast<void**>(&env), JNI_VERSION_1_6);
  if (res == JNI_OK) return env;
  if (res == JNI_EDETACHED) {
    if (g_vm->AttachCurrentThread(&env, nullptr) != JNI_OK) return nullptr;
    *needsDetach = true;
    return env;
  }
  return nullptr;
}

static void postEvent(int eventId, int p1, int p2, int p3, const std::string& s, const std::vector<uint8_t>& data) {
  bool needsDetach = false;
  JNIEnv* env = getEnv(&needsDetach);
  if (!env) return;

  jobject sinkLocal = nullptr;
  jmethodID method = nullptr;
  {
    std::lock_guard<std::mutex> lock(g_mu);
    if (g_sink) sinkLocal = env->NewLocalRef(g_sink);
    method = g_on_event;
  }

  if (sinkLocal && method) {
    jstring js = s.empty() ? nullptr : env->NewStringUTF(s.c_str());
    jbyteArray jdata = nullptr;
    if (!data.empty()) {
      jdata = env->NewByteArray(static_cast<jsize>(data.size()));
      env->SetByteArrayRegion(jdata, 0, static_cast<jsize>(data.size()), reinterpret_cast<const jbyte*>(data.data()));
    }
    env->CallVoidMethod(sinkLocal, method, static_cast<jint>(eventId), static_cast<jint>(p1), static_cast<jint>(p2),
                        static_cast<jint>(p3), js, jdata);
    if (js) env->DeleteLocalRef(js);
    if (jdata) env->DeleteLocalRef(jdata);
  }

  if (sinkLocal) env->DeleteLocalRef(sinkLocal);
  if (needsDetach) g_vm->DetachCurrentThread();
}

extern "C" JNIEXPORT jboolean JNICALL
Java_com_carserial_sdk_NativeBridge_nativeInit(JNIEnv* env, jclass, jstring ttyPath, jobject eventSink) {
  if (!ttyPath || !eventSink) return JNI_FALSE;

  const char* p = env->GetStringUTFChars(ttyPath, nullptr);
  std::string path = p ? p : "";
  if (p) env->ReleaseStringUTFChars(ttyPath, p);

  jclass sinkCls = env->GetObjectClass(eventSink);
  if (!sinkCls) return JNI_FALSE;
  jmethodID mid = env->GetMethodID(sinkCls, "onNativeEvent", "(IIIILjava/lang/String;[B)V");
  if (!mid) return JNI_FALSE;

  {
    std::lock_guard<std::mutex> lock(g_mu);
    if (g_sink) {
      env->DeleteGlobalRef(g_sink);
      g_sink = nullptr;
    }
    g_sink = env->NewGlobalRef(eventSink);
    g_on_event = mid;
  }

  CarSerialService::instance().setEventSink([](int eventId, int p1, int p2, int p3, const std::string& s, const std::vector<uint8_t>& data) {
    postEvent(eventId, p1, p2, p3, s, data);
  });

  bool ok = CarSerialService::instance().init(path);
  return ok ? JNI_TRUE : JNI_FALSE;
}

extern "C" JNIEXPORT void JNICALL
Java_com_carserial_sdk_NativeBridge_nativeDeinit(JNIEnv* env, jclass) {
  CarSerialService::instance().deinit();
  std::lock_guard<std::mutex> lock(g_mu);
  if (g_sink) {
    env->DeleteGlobalRef(g_sink);
    g_sink = nullptr;
  }
  g_on_event = nullptr;
}

extern "C" JNIEXPORT jint JNICALL
Java_com_carserial_sdk_NativeBridge_nativeSendRaw(JNIEnv* env, jclass, jint frameType, jbyteArray payload, jboolean needAck) {
  std::vector<uint8_t> buf;
  if (payload) {
    jsize n = env->GetArrayLength(payload);
    buf.resize(static_cast<size_t>(n));
    env->GetByteArrayRegion(payload, 0, n, reinterpret_cast<jbyte*>(buf.data()));
  }
  return static_cast<jint>(CarSerialService::instance().sendRaw(static_cast<uint8_t>(frameType), buf, needAck == JNI_TRUE));
}

jint JNI_OnLoad(JavaVM* vm, void*) {
  g_vm = vm;
  return JNI_VERSION_1_6;
}

