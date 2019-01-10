// copyright defined in LICENSE.txt

// todo: balance history
// todo: timeout wasm
// todo: timeout sql
// todo: what should memory size limit be?
// todo: check callbacks for recursion to limit stack size
// todo: make sure spidermonkey limits stack size
// todo: global constructors in wasm
// todo: remove block_select and handle in wasm instead
// todo: reformulate get_input_data and set_output_data for reentrancy
// todo: notify wasms of truncated or missing history
// todo: wasms get whether a query is present
// todo: indexes on authorized, ram usage, notify
// todo: namespaces for queries
//          A standard namespace
//          ? one for the tokens
// todo: version on queries
//       vector<extendable<...>>
// todo: version on query api?
// todo: better naming for queries

#include "wasm_ql_plugin.hpp"
#include "queries.hpp"

#include <boost/beast/core.hpp>
#include <boost/beast/http/vector_body.hpp>
#include <boost/beast/websocket.hpp>
#include <boost/signals2/connection.hpp>
#include <fc/exception/exception.hpp>
#include <fc/io/datastream.hpp>
#include <fstream>

#include "jsapi.h"

#include "js/CompilationAndEvaluation.h"
#include "js/Initialization.h"
#include "jsfriendapi.h"

using namespace abieos;
using namespace appbase;
using namespace std::literals;
namespace asio  = boost::asio;
namespace beast = boost::beast;
namespace http  = beast::http;
namespace ws    = boost::beast::websocket;
using tcp       = asio::ip::tcp;

std::string read_string(const char* filename) {
    try {
        std::fstream file(filename, std::ios_base::in | std::ios_base::binary);
        file.seekg(0, std::ios_base::end);
        auto len = file.tellg();
        file.seekg(0, std::ios_base::beg);
        std::string result(len, 0);
        file.read(result.data(), len);
        return result;
    } catch (const std::exception& e) {
        throw std::runtime_error("Error reading "s + filename);
    }
}

static JSClassOps global_ops = {
    nullptr,                  // addProperty
    nullptr,                  // delProperty
    nullptr,                  // enumerate
    nullptr,                  // newEnumerate
    nullptr,                  // resolve
    nullptr,                  // mayResolve
    nullptr,                  // finalize
    nullptr,                  // call
    nullptr,                  // hasInstance
    nullptr,                  // construct
    JS_GlobalObjectTraceHook, // trace
};

static JSClass global_class = {
    "global",
    JSCLASS_GLOBAL_FLAGS,
    &global_ops,
    {},
};

struct context_wrapper {
    JSContext* cx;

    context_wrapper() {
        cx = JS_NewContext(8L * 1024 * 1024);
        if (!cx)
            throw std::runtime_error("JS_NewContext failed");
        if (!JS::InitSelfHostedCode(cx)) {
            JS_DestroyContext(cx);
            throw std::runtime_error("JS::InitSelfHostedCode failed");
        }
    }

    ~context_wrapper() { JS_DestroyContext(cx); }
};

struct block_select {
    enum {
        absolute,
        head,
        irreversible,
    };

    abieos::varuint32 position = {};
    int32_t           offset   = {};
};

template <typename F>
constexpr void for_each_field(block_select*, F f) {
    f("position", member_ptr<&block_select::position>{});
    f("offset", member_ptr<&block_select::offset>{});
}

struct state {
    context_wrapper      context;
    JS::RootedObject     global;
    bool                 console         = {};
    query_config::config config          = {};
    std::string          schema          = {};
    pqxx::connection     sql_connection  = {};
    uint32_t             head            = {};
    abieos::checksum256  head_id         = {};
    uint32_t             irreversible    = {};
    abieos::checksum256  irreversible_id = {};
    input_buffer         request         = {};
    std::vector<char>    reply           = {};

    state()
        : global(context.cx) {
        JS_SetContextPrivate(context.cx, this);
    }

    static state& from_context(JSContext* cx) { return *reinterpret_cast<state*>(JS_GetContextPrivate(cx)); }
};

bool convert_str(JSContext* cx, JSString* str, std::string& dest) {
    auto len = JS_GetStringEncodingLength(cx, str);
    if (len == size_t(-1))
        return false;
    dest.clear();
    dest.resize(len);
    return JS_EncodeStringToBuffer(cx, str, dest.data(), len);
}

bool print_js_str(JSContext* cx, unsigned argc, JS::Value* vp) {
    JS::CallArgs args = CallArgsFromVp(argc, vp);
    std::string  s;
    for (unsigned i = 0; i < args.length(); ++i)
        if (args[i].isString() && convert_str(cx, args[i].toString(), s))
            std::cerr << s;
    return true;
}

bool js_assert(bool cond, JSContext* cx, const char* s) {
    if (!cond)
        JS_ReportErrorUTF8(cx, "%s", s);
    return cond;
}

input_buffer get_input_buffer(JS::CallArgs& args, unsigned buffer_arg, unsigned begin_arg, unsigned end_arg, JS::AutoRequireNoGC& nogc) {
    if (!args[buffer_arg].isObject() || !args[begin_arg].isInt32() || !args[end_arg].isInt32())
        return {};
    JSObject* memory = &args[buffer_arg].toObject();
    auto      begin  = args[begin_arg].toInt32();
    auto      end    = args[end_arg].toInt32();
    if (!JS_IsArrayBufferObject(memory) || !JS_ArrayBufferHasData(memory) || begin < 0 || end < 0 || begin > end)
        return {};
    auto  buf_len = JS_GetArrayBufferByteLength(memory);
    bool  dummy   = false;
    auto* data    = reinterpret_cast<const char*>(JS_GetArrayBufferData(memory, &dummy, nogc));
    if (!data || uint64_t(end) > buf_len)
        return {};
    return {data + begin, data + end};
}

char* get_mem_from_callback(JSContext* cx, JS::CallArgs& args, unsigned callback_arg, int32_t size) {
    JS::RootedValue       cb_result(cx);
    JS::AutoValueArray<1> cb_args(cx);
    cb_args[0].set(JS::NumberValue(size));
    if (!JS_CallFunctionValue(cx, nullptr, args[callback_arg], cb_args, &cb_result) || !cb_result.isObject())
        return nullptr;

    JS::RootedObject cb_result_obj(cx, &cb_result.toObject());
    JS::RootedValue  cb_result_buf(cx);
    JS::RootedValue  cb_result_size(cx);
    if (!JS_GetElement(cx, cb_result_obj, 0, &cb_result_buf) || !JS_GetElement(cx, cb_result_obj, 1, &cb_result_size) ||
        !cb_result_buf.isObject() || !cb_result_size.isInt32())
        return nullptr;

    JS::RootedObject memory(cx, &cb_result_buf.toObject());
    auto             begin = cb_result_size.toInt32();
    auto             end   = uint64_t(begin) + uint64_t(size);
    if (!JS_IsArrayBufferObject(memory) || !JS_ArrayBufferHasData(memory) || begin < 0)
        return nullptr;
    auto buf_len = JS_GetArrayBufferByteLength(memory);
    bool dummy   = false;

    JS::AutoCheckCannotGC checkGC;
    auto                  data = reinterpret_cast<char*>(JS_GetArrayBufferData(memory, &dummy, checkGC));
    if (!data || end > buf_len)
        return nullptr;
    return data + begin;
}

bool print_wasm_str(JSContext* cx, unsigned argc, JS::Value* vp) {
    auto&        state = ::state::from_context(cx);
    JS::CallArgs args  = CallArgsFromVp(argc, vp);
    if (!args.requireAtLeast(cx, "print_wasm_str", 3))
        return false;
    if (!state.console)
        return true;
    {
        JS::AutoCheckCannotGC checkGC;
        auto                  buf = get_input_buffer(args, 0, 1, 2, checkGC);
        if (buf.pos) {
            std::cerr.write(buf.pos, buf.end - buf.pos);
            return true;
        }
    }
    return js_assert(false, cx, "print_wasm_str: invalid args");
}

// args: wasm_name
bool get_wasm(JSContext* cx, unsigned argc, JS::Value* vp) {
    JS::CallArgs args = CallArgsFromVp(argc, vp);
    if (!args.requireAtLeast(cx, "get_wasm", 1))
        return false;
    std::string wasm_name;
    if (!js_assert(args[0].isString() && convert_str(cx, args[0].toString(), wasm_name), cx, "get_wasm: invalid args"))
        return false;

    // todo: sanitize wasm_name
    wasm_name += "-server.wasm";

    try {
        std::fstream file(wasm_name, std::ios_base::in | std::ios_base::binary);
        file.seekg(0, std::ios_base::end);
        auto len = file.tellg();
        if (len <= 0)
            return js_assert(false, cx, ("can not read " + wasm_name).c_str());
        file.seekg(0, std::ios_base::beg);
        auto data = malloc(len);
        if (!data)
            return js_assert(false, cx, ("out of memory reading " + wasm_name).c_str());
        file.read(reinterpret_cast<char*>(data), len);
        JS::CallArgs args = CallArgsFromVp(argc, vp);
        args.rval().setObjectOrNull(JS_NewArrayBufferWithContents(cx, len, data));
        return true;
    } catch (...) {
        return js_assert(false, cx, ("error reading " + wasm_name).c_str());
    }
}

// args: callback
bool get_input_data(JSContext* cx, unsigned argc, JS::Value* vp) {
    auto&        state = ::state::from_context(cx);
    JS::CallArgs args  = CallArgsFromVp(argc, vp);
    if (!args.requireAtLeast(cx, "get_input_data", 1))
        return false;
    try {
        auto size = state.request.end - state.request.pos;
        if (!js_assert((uint32_t)size == size, cx, "get_input_data: request is too big"))
            return false;
        auto data = get_mem_from_callback(cx, args, 0, size);
        if (!js_assert(data, cx, "get_input_data: failed to fetch buffer from callback"))
            return false;
        memcpy(data, state.request.pos, size);
        return true;
    } catch (const std::exception& e) {
        return js_assert(false, cx, ("get_input_data: "s + e.what()).c_str());
    } catch (...) {
        return js_assert(false, cx, "get_input_data error");
    }
} // get_input_data

// args: ArrayBuffer, row_request_begin, row_request_end
bool set_output_data(JSContext* cx, unsigned argc, JS::Value* vp) {
    auto&        state = ::state::from_context(cx);
    JS::CallArgs args  = CallArgsFromVp(argc, vp);
    if (!args.requireAtLeast(cx, "set_output_data", 3))
        return false;
    bool ok = true;
    {
        JS::AutoCheckCannotGC checkGC;
        auto                  b = get_input_buffer(args, 0, 1, 2, checkGC);
        if (b.pos) {
            try {
                state.reply.assign(b.pos, b.end);
            } catch (...) {
                ok = false;
            }
        }
    }
    return js_assert(ok, cx, "set_output_data: invalid args");
}

// args: ArrayBuffer, row_request_begin, row_request_end, callback
bool exec_query(JSContext* cx, unsigned argc, JS::Value* vp) {
    auto&        state = ::state::from_context(cx);
    JS::CallArgs args  = CallArgsFromVp(argc, vp);
    if (!args.requireAtLeast(cx, "exec_query", 4))
        return false;
    std::vector<char> args_bin;
    bool              ok = true;
    {
        JS::AutoCheckCannotGC checkGC;
        auto                  b = get_input_buffer(args, 0, 1, 2, checkGC);
        if (b.pos) {
            try {
                args_bin.assign(b.pos, b.end);
            } catch (...) {
                ok = false;
            }
        }
    }
    if (!ok)
        return js_assert(false, cx, "exec_query: invalid args");

    try {
        abieos::input_buffer args_buf{args_bin.data(), args_bin.data() + args_bin.size()};
        abieos::name         query_name;
        abieos::bin_to_native(query_name, args_buf);

        auto it = state.config.query_map.find(query_name);
        if (it == state.config.query_map.end())
            return js_assert(false, cx, ("exec_query: unknown query: " + (std::string)query_name).c_str());
        query_config::query& query = *it->second;
        query_config::table& table = *query.result_table;

        uint32_t max_block_index = 0;
        if (query.limit_block_index) {
            block_select b;
            abieos::bin_to_native(b, args_buf);
            if (b.position.value == block_select::absolute)
                max_block_index = b.offset;
            else if (b.position.value == block_select::head)
                max_block_index = state.head + b.offset;
            else if (b.position.value == block_select::irreversible)
                max_block_index = state.irreversible + b.offset;
            else
                return js_assert(false, cx, "exec_query: invalid args");
            max_block_index = std::max((int32_t)0, std::min((int32_t)state.head, (int32_t)max_block_index));
        }

        std::string query_str = "select * from \"" + state.schema + "\"." + query.function + "(";
        bool        need_sep  = false;
        if (query.limit_block_index) {
            query_str += sql_conversion::sql_str(max_block_index);
            need_sep = true;
        }
        for (auto& type : query.types) {
            if (need_sep)
                query_str += query_config::sep;
            query_str += type.bin_to_sql(args_buf);
            need_sep = true;
        }
        for (auto& type : query.types) {
            if (need_sep)
                query_str += query_config::sep;
            query_str += type.bin_to_sql(args_buf);
            need_sep = true;
        }
        auto max_results = abieos::read_bin<uint32_t>(args_buf);
        query_str += query_config::sep + sql_conversion::sql_str(std::min(max_results, query.max_results));
        query_str += ")";
        // std::cerr << query_str << "\n";

        pqxx::work        t(state.sql_connection);
        auto              exec_result = t.exec(query_str);
        std::vector<char> result_bin;
        std::vector<char> row_bin;
        push_varuint32(result_bin, exec_result.size());
        for (const auto& r : exec_result) {
            row_bin.clear();
            int i = 0;
            for (auto& type : table.types)
                type.sql_to_bin(row_bin, r[i++]);
            if (!js_assert((uint32_t)row_bin.size() == row_bin.size(), cx, "exec_query: row is too big"))
                return false;
            push_varuint32(result_bin, row_bin.size());
            result_bin.insert(result_bin.end(), row_bin.begin(), row_bin.end());
        }
        t.commit();
        if (!js_assert((uint32_t)result_bin.size() == result_bin.size(), cx, "exec_query: result is too big"))
            return false;
        auto data = get_mem_from_callback(cx, args, 3, result_bin.size());
        if (!js_assert(data, cx, "exec_query: failed to fetch buffer from callback"))
            return false;
        memcpy(data, result_bin.data(), result_bin.size());
        return true;
    } catch (const std::exception& e) {
        return js_assert(false, cx, ("exec_query: "s + e.what()).c_str());
    } catch (...) {
        return js_assert(false, cx, "exec_query error");
    }
} // exec_query

static const JSFunctionSpec functions[] = {
    JS_FN("exec_query", exec_query, 0, 0),           //
    JS_FN("get_input_data", get_input_data, 0, 0),   //
    JS_FN("get_wasm", get_wasm, 0, 0),               //
    JS_FN("print_js_str", print_js_str, 0, 0),       //
    JS_FN("print_wasm_str", print_wasm_str, 0, 0),   //
    JS_FN("set_output_data", set_output_data, 0, 0), //
    JS_FS_END                                        //
};

void init_glue(::state& state) {
    JS::RealmOptions options;
    state.global.set(JS_NewGlobalObject(state.context.cx, &global_class, nullptr, JS::FireOnNewGlobalHook, options));
    if (!state.global)
        throw std::runtime_error("JS_NewGlobalObject failed");

    JSAutoRealm realm(state.context.cx, state.global);
    if (!JS::InitRealmStandardClasses(state.context.cx))
        throw std::runtime_error("JS::InitRealmStandardClasses failed");
    if (!JS_DefineFunctions(state.context.cx, state.global, functions))
        throw std::runtime_error("JS_DefineFunctions failed");
    if (!JS_DefineProperty(state.context.cx, state.global, "global", state.global, 0))
        throw std::runtime_error("JS_DefineProperty failed");

    auto               script   = read_string("../src/glue.js");
    const char*        filename = "noname";
    int                lineno   = 1;
    JS::CompileOptions opts(state.context.cx);
    opts.setFileAndLine(filename, lineno);
    JS::RootedValue rval(state.context.cx);
    bool            ok = JS::EvaluateUtf8(state.context.cx, opts, script.c_str(), script.size(), &rval);
    if (!ok)
        throw std::runtime_error("JS::Evaluate failed");
}

void fetch_fill_status(::state& state) {
    pqxx::work t(state.sql_connection);
    auto       row        = t.exec("select head, head_id, irreversible, irreversible_id from \"" + state.schema + "\".fill_status")[0];
    state.head            = row[0].as<uint32_t>();
    state.head_id         = query_config::sql_to_checksum256(row[1].c_str());
    state.irreversible    = row[2].as<uint32_t>();
    state.irreversible_id = query_config::sql_to_checksum256(row[3].c_str());
}

bool did_fork(::state& state) {
    pqxx::work t(state.sql_connection);
    auto result = t.exec("select block_id from \"" + state.schema + "\".block_info where block_index=" + query_config::sql_str(state.head));
    if (result.empty()) {
        ilog("fork detected (prev head not found)");
        return true;
    }
    auto id = query_config::sql_to_checksum256(result[0][0].c_str());
    if (id.value != state.head_id.value) {
        ilog("fork detected (head_id changed)");
        return true;
    }
    return false;
}

std::vector<char> run(::state& state, const std::vector<char>& request) {
    std::vector<char> result;
    int               num_tries = 0;
    while (true) {
        bool forked = false;
        fetch_fill_status(state);
        input_buffer request_bin{request.data(), request.data() + request.size()};
        auto         num_requests = bin_to_native<varuint32>(request_bin).value;
        result.clear();
        push_varuint32(result, num_requests);
        for (uint32_t request_index = 0; request_index < num_requests; ++request_index) {
            state.request = bin_to_native<input_buffer>(request_bin);
            auto ns_name  = bin_to_native<name>(state.request);
            if (ns_name != "local"_n)
                throw std::runtime_error("unknown namespace: " + (std::string)ns_name);
            auto wasm_name = bin_to_native<name>(state.request);

            JSAutoRealm           realm(state.context.cx, state.global);
            JS::RootedValue       rval(state.context.cx);
            JS::AutoValueArray<1> args(state.context.cx);
            args[0].set(JS::StringValue(JS_NewStringCopyZ(state.context.cx, ((std::string)wasm_name).c_str())));
            if (!JS_CallFunctionName(state.context.cx, state.global, "run", args, &rval)) {
                // todo: detect assert
                JS_ClearPendingException(state.context.cx);
                throw std::runtime_error("JS_CallFunctionName failed");
            }

            if (did_fork(state)) {
                forked = true;
                break;
            }

            push_varuint32(result, state.reply.size());
            result.insert(result.end(), state.reply.begin(), state.reply.end());
        }
        if (!forked)
            return result;
        if (++num_tries >= 4)
            throw std::runtime_error("too many fork events during request");
        ilog("retry request");
    }
}

void fail(beast::error_code ec, char const* what) { elog("${w}: ${s}", ("w", what)("s", ec.message())); }

void handle_request(::state& state, tcp::socket& socket, http::request<http::vector_body<char>> req, beast::error_code& ec) {
    auto const bad_request = [&req](beast::string_view why) {
        http::response<http::string_body> res{http::status::bad_request, req.version()};
        res.set(http::field::server, BOOST_BEAST_VERSION_STRING);
        res.set(http::field::content_type, "text/html");
        res.keep_alive(req.keep_alive());
        res.body() = why.to_string();
        res.prepare_payload();
        return res;
    };

    auto const not_found = [&req](beast::string_view target) {
        http::response<http::string_body> res{http::status::not_found, req.version()};
        res.set(http::field::server, BOOST_BEAST_VERSION_STRING);
        res.set(http::field::content_type, "text/html");
        res.keep_alive(req.keep_alive());
        res.body() = "The resource '" + target.to_string() + "' was not found.\n";
        res.prepare_payload();
        return res;
    };

    auto const server_error = [&req](beast::string_view what) {
        http::response<http::string_body> res{http::status::internal_server_error, req.version()};
        res.set(http::field::server, BOOST_BEAST_VERSION_STRING);
        res.set(http::field::content_type, "text/html");
        res.keep_alive(req.keep_alive());
        res.body() = what.to_string() + "\n";
        res.prepare_payload();
        return res;
    };

    auto const ok = [&req](std::vector<char> reply) {
        http::response<http::vector_body<char>> res{http::status::ok, req.version()};
        res.set(http::field::server, BOOST_BEAST_VERSION_STRING);
        res.set(http::field::content_type, "application/octet-stream");
        res.keep_alive(req.keep_alive());
        res.body() = std::move(reply);
        res.prepare_payload();
        return res;
    };

    auto send = [&](const auto& msg) {
        http::serializer<false, typename std::decay_t<decltype(msg)>::body_type> sr{msg};
        http::write(socket, sr, ec);
    };

    if (req.target() == "/wasmql/v1/query") {
        if (req.method() != http::verb::post)
            return send(bad_request("Unsupported HTTP-method\n"));
        try {
            return send(ok(run(state, req.body())));
        } catch (const std::exception& e) {
            elog("query failed: ${s}", ("s", e.what()));
            return send(server_error("query failed: "s + e.what()));
        } catch (...) {
            elog("query failed: unknown exception");
            return send(server_error("query failed: unknown exception"));
        }
    }

    return send(not_found(req.target()));
}

void accepted(::state& state, tcp::socket socket) {
    beast::error_code ec;

    beast::flat_buffer buffer;
    for (;;) {
        http::request<http::vector_body<char>> req;
        http::read(socket, buffer, req, ec);
        if (ec == http::error::end_of_stream)
            break;
        if (ec)
            return fail(ec, "read");

        handle_request(state, socket, std::move(req), ec);
        if (ec)
            return fail(ec, "write");
    }
    socket.shutdown(tcp::socket::shutdown_send, ec);
}

static abstract_plugin& _wasm_ql_plugin = app().register_plugin<wasm_ql_plugin>();

using boost::signals2::scoped_connection;

template <typename F>
auto catch_and_log(F f) {
    try {
        return f();
    } catch (const fc::exception& e) {
        elog("${e}", ("e", e.to_detail_string()));
    } catch (const std::exception& e) {
        elog("${e}", ("e", e.what()));
    } catch (...) {
        elog("unknown exception");
    }
}

struct wasm_ql_plugin_impl : std::enable_shared_from_this<wasm_ql_plugin_impl> {
    bool                           stopping = false;
    std::string                    endpoint_address;
    std::string                    endpoint_port;
    std::unique_ptr<tcp::acceptor> acceptor;
    std::unique_ptr<::state>       state;

    void listen() {
        boost::system::error_code ec;
        auto                      check_ec = [&](const char* what) {
            if (!ec)
                return;
            elog("${w}: ${m}", ("w", what)("m", ec.message()));
            FC_ASSERT(false, "unable to open listen socket");
        };

        tcp::resolver resolver(app().get_io_service());
        auto          addr = resolver.resolve(tcp::resolver::query(tcp::v4(), endpoint_address, endpoint_port));
        acceptor           = std::make_unique<tcp::acceptor>(app().get_io_service(), *addr.begin(), true);
        acceptor->listen(boost::asio::socket_base::max_listen_connections, ec);
        check_ec("listen");
        do_accept();
    }

    void do_accept() {
        auto socket = std::make_shared<tcp::socket>(app().get_io_service());
        acceptor->async_accept(*socket, [self = shared_from_this(), socket, this](auto ec) {
            if (stopping)
                return;
            if (ec) {
                if (ec == boost::system::errc::too_many_files_open)
                    catch_and_log([&] { do_accept(); });
                return;
            }
            catch_and_log([&] { accepted(*state, std::move(*socket)); });
            catch_and_log([&] { do_accept(); });
        });
    }
}; // wasm_ql_plugin_impl

wasm_ql_plugin::wasm_ql_plugin()
    : my(std::make_shared<wasm_ql_plugin_impl>()) {}

wasm_ql_plugin::~wasm_ql_plugin() { ilog("wasm_ql stopped"); }

void wasm_ql_plugin::set_program_options(options_description& cli, options_description& cfg) {
    auto op = cfg.add_options();
    op("schema,s", bpo::value<std::string>()->default_value("chain"), "Database schema");
    op("query-config,q", bpo::value<std::string>()->default_value("../src/query-config.json"), "Query configuration");
    op("endpoint,e", bpo::value<std::string>()->default_value("localhost:8880"), "Endpoint to listen on");
    op("console,C", "Show console output");
}

void wasm_ql_plugin::plugin_initialize(const variables_map& options) {
    try {
        JS_Init();
        auto ip_port         = options.at("endpoint").as<std::string>();
        my->state            = std::make_unique<::state>();
        my->state->console   = options.count("console");
        my->state->schema    = options["schema"].as<std::string>();
        my->endpoint_port    = ip_port.substr(ip_port.find(':') + 1, ip_port.size());
        my->endpoint_address = ip_port.substr(0, ip_port.find(':'));

        auto x = read_string(options["query-config"].as<std::string>().c_str());
        if (!json_to_native(my->state->config, x))
            throw std::runtime_error("error processing " + options["query-config"].as<std::string>());
        my->state->config.prepare();

        init_glue(*my->state);
    }
    FC_LOG_AND_RETHROW()
}

void wasm_ql_plugin::plugin_startup() { my->listen(); }
void wasm_ql_plugin::plugin_shutdown() { my->stopping = true; }