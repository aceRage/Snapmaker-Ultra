// FCM HTTP v1, the Android half of the push plane (see AppPush.hpp and AppPushProvider.hpp).
//
// Two requests, the first of them cached: a service-account JWT is exchanged for an OAuth2 access
// token, and that token authorises the send. The legacy server-key API was decommissioned in 2024
// and is not an option; v1 needs a Firebase project, whose id is in the send URL.
//
// The message is **data-only, deliberately**. A "notification message" is rendered by the FCM SDK
// itself, which would mean putting cleartext we do not have into the tray; a data message reaches
// FirebaseMessagingService.onMessageReceived, where the app decrypts before anything is shown.
// There must be no `notification` block at all - a message carrying both goes to the tray in the
// background and onMessageReceived is never called, which would silently break the whole design.
//
// None of this needs HTTP/2: the v1 endpoint answers over HTTP/1.1, so Android push works on a
// build of libcurl that cannot reach APNs at all.
#include "AppPushProvider.hpp"

#include "slic3r/Utils/Http.hpp"

#include <boost/log/trivial.hpp>
#include <nlohmann/json.hpp>

#include <cctype>
#include <chrono>
#include <mutex>
#include <string>

namespace Slic3r {
namespace GUI {
namespace AppPush {

using json = nlohmann::json;

static const char* const FCM_SCOPE     = "https://www.googleapis.com/auth/firebase.messaging";
static const char* const FCM_HOST      = "https://fcm.googleapis.com";
static const char* const DEFAULT_TOKEN_URI = "https://oauth2.googleapis.com/token";
static const long long   ASSERTION_LIFETIME = 3600;   // Google's documented maximum
static const long long   TOKEN_EARLY        = 60;     // refresh a minute before it actually expires
static const long        T_CONNECT = 5;
static const long        T_MAX     = 20;

static long long fcm_now_s()
{
    return (long long) std::chrono::duration_cast<std::chrono::seconds>(
               std::chrono::system_clock::now().time_since_epoch()).count();
}

static std::string form_encode(const std::string& s)
{
    static const char* hex = "0123456789ABCDEF";
    std::string        out;
    for (unsigned char c : s) {
        if (std::isalnum(c) || c == '-' || c == '_' || c == '.' || c == '~') out += (char) c;
        else { out += '%'; out += hex[c >> 4]; out += hex[c & 15]; }
    }
    return out;
}

class FcmProvider : public Provider
{
public:
    ~FcmProvider() override { drop_key(); }

    const char* name() const override { return "fcm"; }

    bool available(std::string& why) const override
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        if (!m_enabled) { why = "FCM is switched off"; return false; }
        if (!m_key) { why = m_key_error.empty() ? "no FCM service account is configured" : m_key_error; return false; }
        if (m_client_email.empty()) { why = "the service account JSON has no client_email"; return false; }
        if (m_project_id.empty()) { why = "the FCM project id is not set"; return false; }
        return true;
    }

    void configure(const std::string& config_json) override
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        drop_key();
        m_access_token.clear();
        m_token_expires = 0;
        m_enabled = false;
        m_project_id.clear();
        m_client_email.clear();
        m_token_uri = DEFAULT_TOKEN_URI;
        m_host = FCM_HOST;
        m_key_error.clear();

        json c;
        try { c = json::parse(config_json); } catch (...) { return; }
        if (!c.is_object()) return;
        m_enabled    = c.value("enabled", true);
        m_project_id = c.value("project_id", "");
        if (detail::debug_routes_on()) {
            // The gate's mock FCM, and nothing else: honoured only with SNORCA_DEBUG_ROUTES=1.
            const std::string h = c.value("host_override", "");
            const std::string t = c.value("token_uri_override", "");
            if (!h.empty()) m_host = h;
            if (!t.empty()) m_token_uri = t;
        }

        // The service account is read here, once, and the parsed key is kept: a push at 03:00
        // must not fail because the file moved. The path stays the source of truth and this is a
        // cache, refreshed whenever the settings change.
        std::string blob = c.value("service_account_json", "");
        if (blob.empty()) {
            const std::string path = c.value("service_account_path", "");
            if (path.empty()) { m_key_error = "no FCM service account is configured"; return; }
            std::string err;
            if (!detail::read_text_file(path, blob, err)) { m_key_error = err; return; }
        }
        json sa;
        try { sa = json::parse(blob); } catch (...) { m_key_error = "the service account file is not JSON"; return; }
        if (!sa.is_object()) { m_key_error = "the service account file is not a JSON object"; return; }
        m_client_email = sa.value("client_email", "");
        if (m_project_id.empty()) m_project_id = sa.value("project_id", "");
        const std::string uri = sa.value("token_uri", "");
        if (!uri.empty() && m_token_uri == DEFAULT_TOKEN_URI) m_token_uri = uri;
        const std::string pem = sa.value("private_key", "");
        if (pem.empty()) { m_key_error = "the service account JSON has no private_key"; return; }
        std::string err;
        m_key = detail::load_pkcs8_pem(pem, err);
        if (!m_key) m_key_error = err;
    }

    PushResult send(const PushRequest& req) override
    {
        PushResult res;
        std::string token, project, host;
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            project = m_project_id;
            host    = m_host;
        }
        res.host = host;
        std::string err;
        if (!access_token(token, err)) { res.error = err; return res; }

        // Data-only, with no `notification` block: see the header comment. `v` lets the app tell
        // envelope versions apart if this ever changes.
        json msg;
        msg["message"]["token"]        = req.device_token;
        msg["message"]["data"]["e"]    = req.ciphertext_b64u;
        msg["message"]["data"]["v"]    = "1";
        msg["message"]["data"]["c"]    = req.collapse_id;
        msg["message"]["android"]["priority"] = req.priority >= 10 ? "high" : "normal";
        msg["message"]["android"]["ttl"]      = std::to_string(req.ttl_seconds) + "s";
        // The collapse key does on Android what apns-collapse-id does on iOS: a second "paused"
        // for the same printer replaces the first rather than stacking.
        msg["message"]["android"]["collapse_key"] = req.collapse_id;
        const std::string body = msg.dump();

        const std::string url = host + "/v1/projects/" + project + "/messages:send";
        std::string       answer;
        Http req_http = Http::post(url);
        req_http.timeout_connect(T_CONNECT).timeout_max(T_MAX)
            .header("Authorization", "Bearer " + token)
            .header("Content-Type", "application/json")
            .set_post_body(body)
            .on_complete([&](std::string b, unsigned st) { res.ok = true; res.status = (int) st; answer = b; })
            .on_error([&](std::string b, std::string e, unsigned st) {
                res.status = (int) st;
                res.error  = e.empty() ? ("HTTP " + std::to_string(st)) : e;
                answer     = b;
            })
            .perform_sync();

        if (!res.ok) classify(res, answer);
        if (!res.ok && res.error.empty()) res.error = "FCM did not answer";
        return res;
    }

private:
    void drop_key()
    {
        if (m_key) { detail::free_pkey(m_key); m_key = nullptr; }
    }

    // Google's error shape is {"error":{"status":"UNREGISTERED", ...}}; the FCM-specific detail is
    // in error.details[].errorCode. Both are checked, because which one carries UNREGISTERED has
    // moved between releases and pruning a live device by mistake is the expensive failure.
    void classify(PushResult& res, const std::string& answer)
    {
        std::string code;
        try {
            const json j = json::parse(answer);
            if (j.is_object() && j.contains("error") && j["error"].is_object()) {
                code = j["error"].value("status", "");
                if (j["error"].contains("details") && j["error"]["details"].is_array())
                    for (const auto& d : j["error"]["details"])
                        if (d.is_object() && d.contains("errorCode") && d["errorCode"].is_string())
                            code = d["errorCode"].get<std::string>();
                if (res.error.empty() || res.error.compare(0, 5, "HTTP ") == 0) {
                    const std::string msg = j["error"].value("message", "");
                    if (!msg.empty()) res.error = msg;
                }
            }
        } catch (...) {}
        if (!code.empty()) res.error = code + (res.error.empty() ? "" : (": " + res.error));
        // The app was uninstalled or the token was replaced. Nothing retries this and the row goes.
        if (res.status == 404 && (code == "UNREGISTERED" || code == "NOT_FOUND" || code.empty()))
            res.gone = true;
        // The access token aged out mid-flight. This must never look like a dead device: AppPush
        // re-mints and tries exactly once more.
        if (res.status == 401) res.credential_expired = true;
    }

    // The OAuth2 access token, minted from an RS256 service-account assertion and kept until it
    // is nearly expired. One exchange an hour, not one per notification.
    bool access_token(std::string& out, std::string& err)
    {
        std::string uri, email, project, key_error;
        void*       key = nullptr;
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            if (!m_enabled) { err = "FCM is switched off"; return false; }
            if (!m_access_token.empty() && m_token_expires - fcm_now_s() > TOKEN_EARLY) {
                out = m_access_token;
                return true;
            }
            uri      = m_token_uri;
            email    = m_client_email;
            project  = m_project_id;
            key      = m_key;
            key_error = m_key_error;
        }
        if (!key) { err = key_error.empty() ? "no FCM service account is configured" : key_error; return false; }
        if (email.empty() || project.empty()) { err = "the FCM service account is incomplete"; return false; }

        const long long iat = fcm_now_s();
        json            claims;
        claims["iss"]   = email;
        claims["scope"] = FCM_SCOPE;
        claims["aud"]   = uri;
        claims["iat"]   = iat;
        claims["exp"]   = iat + ASSERTION_LIFETIME;
        std::string assertion;
        if (!detail::sign_jwt(key, /*es256=*/false,
                              json({ { "alg", "RS256" }, { "typ", "JWT" } }).dump(),
                              claims.dump(), assertion, err))
            return false;

        const std::string form = "grant_type=" + form_encode("urn:ietf:params:oauth:grant-type:jwt-bearer") +
                                 "&assertion=" + form_encode(assertion);
        std::string answer;
        bool        ok = false;
        unsigned    status = 0;
        Http::post(uri)
            .timeout_connect(T_CONNECT).timeout_max(T_MAX)
            .header("Content-Type", "application/x-www-form-urlencoded")
            .set_post_body(form)
            .on_complete([&](std::string b, unsigned st) { ok = true; status = st; answer = b; })
            .on_error([&](std::string b, std::string e, unsigned st) { status = st; answer = b; err = e; })
            .perform_sync();
        if (!ok) {
            if (err.empty()) err = "the OAuth2 token endpoint answered HTTP " + std::to_string(status);
            // Never let a token-endpoint failure carry the assertion back into a log line.
            return false;
        }
        try {
            const json j = json::parse(answer);
            out = j.value("access_token", "");
            const long long ttl = j.value("expires_in", 3600LL);
            if (out.empty()) { err = "the OAuth2 token endpoint returned no access_token"; return false; }
            std::lock_guard<std::mutex> lock(m_mutex);
            m_access_token  = out;
            m_token_expires = fcm_now_s() + ttl;
        } catch (...) {
            err = "the OAuth2 token endpoint did not answer JSON";
            return false;
        }
        return true;
    }

    mutable std::mutex m_mutex;
    bool               m_enabled { false };
    void*              m_key { nullptr };
    std::string        m_key_error;
    std::string        m_client_email, m_project_id;
    std::string        m_token_uri { DEFAULT_TOKEN_URI };
    std::string        m_host { FCM_HOST };
    std::string        m_access_token;
    long long          m_token_expires { 0 };
};

std::unique_ptr<Provider> make_fcm_provider() { return std::unique_ptr<Provider>(new FcmProvider()); }

} // namespace AppPush
} // namespace GUI
} // namespace Slic3r
