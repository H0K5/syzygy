// Copyright 2014 Google Inc. All Rights Reserved.
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

#include "syzygy/kasko/http_agent_impl.h"

#include <windows.h>
#include <winhttp.h>

#include <memory>
#include <string>

#include "base/file_version_info.h"
#include "base/logging.h"
#include "base/macros.h"
#include "base/sys_info.h"
#include "base/strings/string16.h"
#include "base/win/scoped_handle.h"
#include "base/win/windows_version.h"
#include "syzygy/common/com_utils.h"
#include "syzygy/kasko/http_response.h"
#include "syzygy/kasko/internet_helpers.h"
#include "syzygy/kasko/user_agent.h"

namespace kasko {

namespace {

class WinHttpHandleTraits {
 public:
  typedef HINTERNET Handle;
  static bool CloseHandle(HINTERNET handle) {
    return ::WinHttpCloseHandle(handle) == TRUE;
  }
  static bool IsHandleValid(HINTERNET handle) { return handle != nullptr; }
  static HINTERNET NullHandle() { return nullptr; }

 private:
  DISALLOW_IMPLICIT_CONSTRUCTORS(WinHttpHandleTraits);
};

typedef base::win::GenericScopedHandle<WinHttpHandleTraits,
                                       base::win::DummyVerifierTraits>
    ScopedWinHttpHandle;

// A helper class that retrieves and frees user proxy settings.
class AutoWinHttpProxyConfig {
 public:
  AutoWinHttpProxyConfig() : proxy_config_() {}
  ~AutoWinHttpProxyConfig() {
    if (proxy_config_.lpszAutoConfigUrl)
      ::GlobalFree(proxy_config_.lpszAutoConfigUrl);
    if (proxy_config_.lpszProxy)
      ::GlobalFree(proxy_config_.lpszProxy);
    if (proxy_config_.lpszProxyBypass)
      ::GlobalFree(proxy_config_.lpszProxyBypass);
  }

  // Attempts to load the user proxy settings. If successful, returns true.
  bool Load() {
    if (::WinHttpGetIEProxyConfigForCurrentUser(&proxy_config_))
      return true;

    LOG(ERROR) << "WinHttpGetIEProxyConfigForCurrentUser() failed: "
               << ::common::LogWe();
    return false;
  }

  DWORD access_type() const {
    return (proxy() == WINHTTP_NO_PROXY_NAME) ? WINHTTP_ACCESS_TYPE_NO_PROXY
                                              : WINHTTP_ACCESS_TYPE_NAMED_PROXY;
  }

  // Indicates whether proxy auto-detection is enabled.
  const bool auto_detect() const { return proxy_config_.fAutoDetect == TRUE; }

  // Returns the proxy auto-configuration URL, or an empty string if automatic
  // proxy configuration is disabled. Only valid after a successful call to
  // Load().
  const base::char16* auto_config_url() const {
    return proxy_config_.lpszAutoConfigUrl ? proxy_config_.lpszAutoConfigUrl
                                           : L"";
  }

  // Returns the proxy configuration string that should be passed to
  // WinHttpOpen.
  const base::char16* proxy() const {
    return (proxy_config_.lpszProxy && proxy_config_.lpszProxy[0] != 0)
               ? proxy_config_.lpszProxy
               : WINHTTP_NO_PROXY_NAME;
  }

  // Returns the proxy bypass configuration string that should be passed to
  // WinHttpOpen. Only valid after a successful call to Load().
  const base::char16* proxy_bypass() const {
    return access_type() == WINHTTP_ACCESS_TYPE_NO_PROXY
               ? WINHTTP_NO_PROXY_BYPASS
               : proxy_config_.lpszProxyBypass;
  }

 private:
  WINHTTP_CURRENT_USER_IE_PROXY_CONFIG proxy_config_;

  DISALLOW_COPY_AND_ASSIGN(AutoWinHttpProxyConfig);
};

// A helper class that retrieves and frees URL-specific proxy settings.
class AutoWinHttpUrlProxyConfig {
 public:
  // Constructs an instance that will use the auto-configuration URL (if any)
  // from |proxy_config| to retrieve URL-specific proxy settings.
  explicit AutoWinHttpUrlProxyConfig(const AutoWinHttpProxyConfig& proxy_config)
      : auto_detect_(proxy_config.auto_detect()),
        auto_config_url_(proxy_config.auto_config_url()),
        is_valid_(false),
        url_proxy_config_() {}

  ~AutoWinHttpUrlProxyConfig() {
    if (url_proxy_config_.lpszProxy)
      ::GlobalFree(url_proxy_config_.lpszProxy);
    if (url_proxy_config_.lpszProxyBypass)
      ::GlobalFree(url_proxy_config_.lpszProxyBypass);
  }

  // Loads URL-specific proxy settings for |url| using |session|. Returns true
  // if auto-configuration is disabled or if the settings are successfully
  // loaded.
  bool Load(HINTERNET session, const base::string16& url) {
    // http://msdn.microsoft.com/en-us/library/fze2ytx2(v=vs.110).aspx implies
    // that auto-detection is to be used before a specified configuration file.

    // TODO(erikwright): It's not clear if an error from WinHttpGetProxyForUrl
    // means that no proxy is detected and we should proceed with a direct
    // connection or that something unexpected happened. In the latter case we
    // should presumably log an error and possibly not attempt a direct
    // connection. Manual testing will be required to verify the behaviour of
    // this code in different proxy scenarios.
    if (auto_detect_) {
      WINHTTP_AUTOPROXY_OPTIONS options = {0};
      options.dwFlags =
          WINHTTP_AUTOPROXY_AUTO_DETECT | WINHTTP_AUTOPROXY_RUN_OUTPROCESS_ONLY;
      options.dwAutoDetectFlags =
          WINHTTP_AUTO_DETECT_TYPE_DHCP | WINHTTP_AUTO_DETECT_TYPE_DNS_A;
      if (::WinHttpGetProxyForUrl(session, url.c_str(), &options,
                                  &url_proxy_config_)) {
        is_valid_ = true;
        return true;
      }

      switch (::GetLastError()) {
        case ERROR_WINHTTP_AUTODETECTION_FAILED:
        case ERROR_WINHTTP_AUTO_PROXY_SERVICE_ERROR:
          break;
        default:
          LOG(ERROR) << "Unexpected error durring "
                        "WinHttpGetProxyForUrl(WINHTTP_AUTOPROXY_AUTO_DETECT)"
                     << ::common::LogWe();
          return false;
      }
    }

    // Auto-detection is disabled or did not detect a configuration.
    if (!auto_config_url_.empty()) {
      WINHTTP_AUTOPROXY_OPTIONS options = {0};
      options.dwFlags = WINHTTP_AUTOPROXY_CONFIG_URL;
      options.lpszAutoConfigUrl = auto_config_url_.c_str();

      if (::WinHttpGetProxyForUrl(session, url.c_str(), &options,
                                  &url_proxy_config_)) {
        is_valid_ = true;
        return true;
      }

      LOG(ERROR)
          << "WinHttpGetProxyForUrl(WINHTTP_AUTOPROXY_CONFIG_URL) failed: "
          << ::common::LogWe();
      return false;
    }
    return true;
  }

  // Returns the loaded settings, or NULL if auto-configuration is disabled.
  // Only valid after a successful call to Load().
  WINHTTP_PROXY_INFO* get() {
    return is_valid_ ? &url_proxy_config_ : nullptr;
  }

 private:
  bool auto_detect_;
  base::string16 auto_config_url_;
  bool is_valid_;
  WINHTTP_PROXY_INFO url_proxy_config_;

  DISALLOW_COPY_AND_ASSIGN(AutoWinHttpUrlProxyConfig);
};

// Implements HttpResponse using the WinHTTP API.
class HttpResponseImpl : public HttpResponse {
 public:
  ~HttpResponseImpl() override;

  // Issues the request defined by its parameters and, if successful, returns an
  // HttpResponse that may be used to access the response. See HttpAgent::Post
  // for a description of the parameters.
  static std::unique_ptr<HttpResponse> Create(
      const base::string16& user_agent,
      const base::string16& host,
      uint16_t port,
      const base::string16& path,
      bool secure,
      const base::string16& extra_headers,
      const std::string& body);

  // HttpResponse implementation.
  bool GetStatusCode(uint16_t* status_code) override;
  bool GetContentLength(bool* has_content_length,
                        size_t* content_length) override;
  bool GetContentType(bool* has_content_type,
                      base::string16* content_type) override;
  bool HasData(bool* has_data) override;
  bool ReadData(char* buffer, size_t* count) override;

 private:
  HttpResponseImpl();

  // Invokes WinHttpQueryHeaders. If the header indicated by |info_level| is
  // present its value will be read into |buffer| (having size |buffer_length|).
  // |header_present| will indicate whether the header was found. The result is
  // true if the header is successfully read or determined to be absent.
  bool QueryHeader(DWORD info_level,
                   bool* header_present,
                   void* buffer,
                   DWORD buffer_length);

  // WinHttp handles used for the request.
  ScopedWinHttpHandle session_;
  ScopedWinHttpHandle connection_;
  ScopedWinHttpHandle request_;

  DISALLOW_COPY_AND_ASSIGN(HttpResponseImpl);
};

HttpResponseImpl::~HttpResponseImpl() {}

// static
std::unique_ptr<HttpResponse> HttpResponseImpl::Create(
    const base::string16& user_agent,
    const base::string16& host,
    uint16_t port,
    const base::string16& path,
    bool secure,
    const base::string16& extra_headers,
    const std::string& body) {
  // Retrieve the user's proxy configuration.
  AutoWinHttpProxyConfig proxy_config;
  if (!proxy_config.Load())
    return std::unique_ptr<HttpResponse>();

  // Tentatively create an instance. We will return it if we are able to
  // successfully initialize it.
  std::unique_ptr<HttpResponseImpl> instance(new HttpResponseImpl);

  // Open a WinHTTP session.
  instance->session_.Set(
      ::WinHttpOpen(user_agent.c_str(), proxy_config.access_type(),
                    proxy_config.proxy(), proxy_config.proxy_bypass(), 0));
  if (!instance->session_.IsValid()) {
    LOG(ERROR) << "WinHttpOpen() failed: " << ::common::LogWe();
    return std::unique_ptr<HttpResponse>();
  }

  // Look up URL-specific proxy settings. If this fails, we will fall back to
  // working without a proxy.
  AutoWinHttpUrlProxyConfig url_proxy_config(proxy_config);
  url_proxy_config.Load(instance->session_.Get(),
                        ComposeUrl(host, port, path, secure));

  // Connect to a host/port.
  instance->connection_.Set(
      ::WinHttpConnect(instance->session_.Get(), host.c_str(), port, 0));
  if (!instance->connection_.IsValid()) {
    LOG(ERROR) << "WinHttpConnect() failed with host " << host << " and port "
               << port << ": " << ::common::LogWe();
    return std::unique_ptr<HttpResponse>();
  }

  // Initiate a request. This doesn't actually send the request yet.
  instance->request_.Set(
      ::WinHttpOpenRequest(instance->connection_.Get(), L"POST", path.c_str(),
                           NULL,  // version
                           NULL,  // referer
                           NULL,  // accept types
                           secure ? WINHTTP_FLAG_SECURE : 0));
  if (!instance->connection_.IsValid()) {
    LOG(ERROR) << "WinHttpConnect() failed with host " << host << " and port "
               << port << ": " << ::common::LogWe();
    return std::unique_ptr<HttpResponse>();
  }

  // Disable cookies and authentication. This request should be completely
  // stateless and untied to any identity of any sort.
  DWORD option_value = WINHTTP_DISABLE_COOKIES | WINHTTP_DISABLE_AUTHENTICATION;
  if (!::WinHttpSetOption(instance->request_.Get(),
                          WINHTTP_OPTION_DISABLE_FEATURE, &option_value,
                          sizeof(option_value))) {
    LOG(ERROR) << "WinHttpSetOption(WINHTTP_DISABLE_COOKIES | "
                  "WINHTTP_DISABLE_AUTHENTICATION) failed: "
               << ::common::LogWe();
    return std::unique_ptr<HttpResponse>();
  }

  // If this URL is configured to use a proxy, set that up now.
  if (url_proxy_config.get()) {
    if (!::WinHttpSetOption(instance->request_.Get(), WINHTTP_OPTION_PROXY,
                            url_proxy_config.get(),
                            sizeof(*url_proxy_config.get()))) {
      LOG(ERROR) << "WinHttpSetOption(WINHTTP_OPTION_PROXY) failed: "
                 << ::common::LogWe();
      return std::unique_ptr<HttpResponse>();
    }
  }

  // Send the request.
  if (!::WinHttpSendRequest(instance->request_.Get(), extra_headers.c_str(),
                            static_cast<DWORD>(-1),
                            const_cast<char*>(body.data()),
                            static_cast<DWORD>(body.size()),
                            static_cast<DWORD>(body.size()), NULL)) {
    LOG(ERROR) << "Failed to send HTTP request to host " << host << " and port "
               << port << ": " << ::common::LogWe();
    return std::unique_ptr<HttpResponse>();
  }

  // This seems to read at least all headers from the response. The remainder of
  // the body, if any, may be read during subsequent calls to WinHttpReadData().
  if (!::WinHttpReceiveResponse(instance->request_.Get(), 0)) {
    LOG(ERROR) << "Failed to complete HTTP request to host " << host
               << " and port " << port << ": " << ::common::LogWe();
    return std::unique_ptr<HttpResponse>();
  }

  return std::move(instance);
}

bool HttpResponseImpl::GetStatusCode(uint16_t* status_code) {
  DCHECK(status_code);

  bool has_status_code = false;
  DWORD status_code_buffer = 0;

  if (QueryHeader(WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
                  &has_status_code, &status_code_buffer,
                  sizeof(status_code_buffer))) {
    DCHECK(has_status_code);
    *status_code = status_code_buffer;
    return true;
  }

  return false;
}

bool HttpResponseImpl::GetContentLength(bool* has_content_length,
                                        size_t* content_length) {
  DCHECK(has_content_length);
  DCHECK(content_length);

  DWORD content_length_header_value = 0;

  if (QueryHeader(WINHTTP_QUERY_CONTENT_LENGTH | WINHTTP_QUERY_FLAG_NUMBER,
                  has_content_length, &content_length_header_value,
                  sizeof(content_length_header_value))) {
    if (has_content_length)
      *content_length = content_length_header_value;
    return true;
  }

  return false;
}

bool HttpResponseImpl::GetContentType(bool* has_content_type,
                                      base::string16* content_type) {
  DCHECK(has_content_type);
  DCHECK(content_type);

  base::char16 content_type_buffer[256] = {0};

  if (QueryHeader(WINHTTP_QUERY_CONTENT_TYPE, has_content_type,
                  &content_type_buffer, sizeof(content_type_buffer))) {
    if (has_content_type)
      *content_type = content_type_buffer;
    return true;
  }

  return false;
}

bool HttpResponseImpl::HasData(bool* has_data) {
  DCHECK(has_data);

  DWORD leftover_data = 0;
  if (!::WinHttpQueryDataAvailable(request_.Get(), &leftover_data)) {
    LOG(ERROR) << "WinHttpQueryDataAvailable failed: " << ::common::LogWe();
    return false;
  }
  if (leftover_data != 0)
    *has_data = true;
  else
    *has_data = false;
  return true;
}

bool HttpResponseImpl::ReadData(char* buffer, size_t* count) {
  DCHECK(buffer);
  DCHECK(count);

  DWORD size_read = 0;
  if (!::WinHttpReadData(request_.Get(), buffer, *count, &size_read)) {
    LOG(ERROR) << "Failed to read response body: " << ::common::LogWe();
    return false;
  }
  *count = size_read;
  return true;
}

HttpResponseImpl::HttpResponseImpl() {}

bool HttpResponseImpl::QueryHeader(DWORD info_level,
                                   bool* header_present,
                                   void* buffer,
                                   DWORD buffer_length) {
  if (::WinHttpQueryHeaders(request_.Get(), info_level,
                            WINHTTP_HEADER_NAME_BY_INDEX, buffer,
                            &buffer_length, 0)) {
    *header_present = true;
    return true;
  }

  if (::GetLastError() == ERROR_WINHTTP_HEADER_NOT_FOUND) {
    *header_present = false;
    return true;
  }

  LOG(ERROR) << "WinHttpQueryHeaders failed:" << ::common::LogWe();
  return false;
}

base::string16 GetWinHttpVersion() {
  HMODULE win_http_module = nullptr;
  if (::GetModuleHandleEx(0, L"winhttp.dll", &win_http_module)) {
    std::unique_ptr<FileVersionInfo> win_http_module_version_info(
        FileVersionInfo::CreateFileVersionInfoForModule(win_http_module));
    ::FreeLibrary(win_http_module);
    if (win_http_module_version_info)
      return win_http_module_version_info->product_version();
  }
  return L"?";
}

// Adapted from Chromium content/common/user_agent.cc
void GetOSAndCPU(UserAgent* user_agent) {
  int32_t os_major_version = 0;
  int32_t os_minor_version = 0;
  int32_t os_bugfix_version = 0;
  base::SysInfo::OperatingSystemVersionNumbers(&os_major_version,
                                               &os_minor_version,
                                               &os_bugfix_version);
  user_agent->set_os_version(os_major_version, os_minor_version);

  base::win::OSInfo* os_info = base::win::OSInfo::GetInstance();
  if (os_info->wow64_status() == base::win::OSInfo::WOW64_ENABLED) {
    user_agent->set_architecture(UserAgent::WOW64);
  } else {
    base::win::OSInfo::WindowsArchitecture windows_architecture =
        os_info->architecture();
    if (windows_architecture == base::win::OSInfo::X64_ARCHITECTURE)
      user_agent->set_architecture(UserAgent::X64);
    else if (windows_architecture == base::win::OSInfo::IA64_ARCHITECTURE)
      user_agent->set_architecture(UserAgent::IA64);
    else
      user_agent->set_architecture(UserAgent::X86);
  }
}

}  // namespace

HttpAgentImpl::HttpAgentImpl(const base::string16& product_name,
                             const base::string16& product_version) {
  UserAgent user_agent(product_name, product_version);
  user_agent.set_winhttp_version(GetWinHttpVersion());
  GetOSAndCPU(&user_agent);
  user_agent_ = user_agent.AsString();
}

HttpAgentImpl::~HttpAgentImpl() {}

std::unique_ptr<HttpResponse> HttpAgentImpl::Post(
    const base::string16& host,
    uint16_t port,
    const base::string16& path,
    bool secure,
    const base::string16& extra_headers,
    const std::string& body) {
  return HttpResponseImpl::Create(user_agent_, host, port, path, secure,
                                  extra_headers, body);
}

}  // namespace kasko
