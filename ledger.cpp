#include "../date/include/date/date.h"
#include <cmath>
#include <memory>
#include <string>
#include <curl/curl.h>
#include <json/json.h>

// CURL tools

static
int
curl_global()
{
    if (::curl_global_init(CURL_GLOBAL_DEFAULT) != 0)
        throw std::runtime_error("CURL global initialization failed");
    return 0;
}

namespace
{

struct curl_deleter
{
    void operator()(CURL* p) const
    {
        ::curl_easy_cleanup(p);
    }
};

}  // unnamed namespace

static
std::unique_ptr<CURL, curl_deleter>
curl_init()
{
    static const auto curl_is_now_initiailized = curl_global();
    (void)curl_is_now_initiailized;
    return std::unique_ptr<CURL, curl_deleter>{::curl_easy_init()};
}

static
bool
post_and_download_to_string(const std::string& url, std::string const& post,
                            std::string& reply)
{
    reply.clear();
    auto curl = curl_init();
    if (!curl)
        return false;
    std::string version;
    curl_easy_setopt(curl.get(), CURLOPT_USERAGENT, "curl");
    curl_easy_setopt(curl.get(), CURLOPT_URL, url.c_str());
    curl_write_callback write_cb = [](char* contents, std::size_t size, std::size_t nmemb,
                                      void* userp) -> std::size_t
    {
        auto& userstr = *static_cast<std::string*>(userp);
        auto realsize = size * nmemb;
        userstr.append(contents, realsize);
        return realsize;
    };
    curl_easy_setopt(curl.get(), CURLOPT_WRITEFUNCTION, write_cb);
    curl_easy_setopt (curl.get(), CURLOPT_POSTFIELDSIZE, post.size());
    curl_easy_setopt (curl.get(), CURLOPT_POSTFIELDS, post.c_str());
    curl_easy_setopt(curl.get(), CURLOPT_WRITEDATA, &reply);
    curl_easy_setopt(curl.get(), CURLOPT_SSL_VERIFYPEER, false);
    auto res = curl_easy_perform(curl.get());
    return (res == CURLE_OK);
}

// Execute a query against the S2 cluster of full history XRP Ledger notes.
// Note that this is a best-effort service that does not guarantee
// any particular level of reliability.
bool
do_query(std::string const& method, Json::Value const& params, Json::Value& reply)
{
    std::string q;

    {
        Json::Value query = Json::objectValue;
        query["method"] = method;
        Json::Value& p = (query["params"] = Json::arrayValue);
        p.append(params);

        Json::FastWriter w;
        q = w.write (query);
    }

    std::string out;
    if (!post_and_download_to_string("http://s2.ripple.com:51234", q, out))
        return false;

    Json::Reader reader;
    Json::Value root;
    if (!reader.parse(out.c_str(), root))
    {
        std::cerr << reader.getFormatedErrorMessages() << '\n';
        return false;
    }
    Json::Value& result = root["result"];
    if (! result.isObject())
    {
        std::cerr << "Result is not object\n";
        return false;
    }
    Json::Value& status = result["status"];
    if (!status.isString() || (status.asString() != "success"))
    {
        std::cerr << "Result is '" << status.asString() << "', not success\n";
        reply = std::move(result);
        return false;
    }

    reply = std::move(root);
    return true;
}

// Get the header of a ledger given its sequence number
bool getHeader (unsigned ledger_seq, Json::Value& header)
{
    Json::Value reply;
    Json::Value params = Json::objectValue;
    if (ledger_seq == 0)
        params["ledger_index"] = "validated";
    else
        params["ledger_index"] = ledger_seq;
    if (!do_query("ledger", params, reply))
    {
        header = std::move(reply);
        return false;
    }
    header = reply["result"]["ledger"];
    if (header.isObject() && !header.isNull())
        return true;
    header = std::move(reply);
    return false;
}

std::pair<int, int>
get_last_validated_close_time()
{
    Json::Value reply;
    if (getHeader(0, reply))
        return {reply["ledger_index"].asInt(), reply["close_time"].asInt()};
    return {0, 0};
}

int
get_close_time(unsigned ledger_seq)
{
    Json::Value reply;
    if (getHeader(ledger_seq, reply))
        return reply["close_time"].asInt();
    return 0;
}

int
main()
{
    using namespace std::chrono;
    using namespace date;
    constexpr sys_seconds epoch = sys_days{2000_y/1/1};

    auto target = sys_days{2020_y/1/1} - 1s -  epoch;
    std::cout << std::fixed;
    std::cout << "Looking for {ledger at, " << target/1.s << ", " << target+epoch << "}\n";

    auto [l2, t2] = get_last_validated_close_time();
    std::cout << '{' << l2 << ", " << t2 << ", " << seconds{t2}+epoch << "}\n";
    if (seconds{t2} == target)
        return 0;
    auto l1 = l2 - 10;
    int t1;
    auto nl = l1;
    int* pnt = &t1;  // pointer to new guess' timestamp

    while (true)
    {
        *pnt = get_close_time(nl);
        std::cout << '{' << nl << ", " << *pnt << ", " << seconds{*pnt}+epoch << "}\n";
        // invariant: l1 < l2, t1 < t2
        if (seconds{*pnt} == target)
        {
            l1 = nl;
            t1 = *pnt;
            break;
        }

        auto m = double(l2-l1)/(t2-t1);
        auto b = l1 - m*t1;
        nl = static_cast<int>(std::round(m*(target/1s) + b));

        if (nl < l1)
        {   // If the guess is extrapolated below, chase it with our worst previous guess
            l2 = l1;
            t2 = t1;
            l1 = nl;
            pnt = &t1;
        }
        else if (nl > l2)
        {   // If the guess is extrapolated above, chase it with our worst previous guess
            l1 = l2;
            t1 = t2;
            l2 = nl;
            pnt = &t2;
        }
        else if (nl == l1)
        {   // If the guess is the lower bound and the upper bound is one away
            if (l2 - l1 == 1)
                break;  // The answer is the lower bound
            // Else set the upper bound to one above the lower bound and try again
            l2 = nl = l1 + 1;
            pnt = &t2;
        }
        else if (nl == l2)
        {   // If the guess is the upper bound and the lower bound is one away
            if (l1 == l2-1)
            {   // The answer is the upper bound
                l1 = l2;
                t1 = t2;
                break;
            }
            // Else set the lower bound to one below the upper bound and try again
            l1 = nl = l2 - 1;
            pnt = &t1;
        }
        else  // Else the guess is interpolated between the lower and upper bounds
        {   // Move the worst guess to the new guess and try again
            if (nl - l1 <= l2 - nl)
            {
                l2 = nl;
                pnt = &t2;
            }
            else
            {
                l1 = nl;
                pnt = &t1;
            }
        }
    }
    std::cout << "---\n"
              << '{' << l1 << ", " << t1 << ", " << seconds{t1}+epoch << "}\n";
}
