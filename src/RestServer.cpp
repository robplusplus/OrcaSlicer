// Minimal synchronous Boost.Beast HTTP server that listens on localhost and
// handles a small JSON RPC: POST /rpc -> {"action":"import"|"slice_plate", ...}

#include "RestServer.hpp"

// If the project has not defined _WIN32_WINNT in CMake, define a sensible default
#if defined(_WIN32) && !defined(_WIN32_WINNT)
#define _WIN32_WINNT 0x0601
#endif

#include <boost/asio.hpp>
#include <boost/beast/core.hpp>
#include <boost/beast/http.hpp>
#include <boost/beast/version.hpp>

#include <iostream>
#include <vector>
#include <string>
#include <memory>
#include <sstream>
#include <cctype>

#include "nlohmann/json.hpp"

// Use project logging
#include <boost/log/trivial.hpp>

// Action registry
#include "slic3r/Utils/ActionRegister.hpp"
#include "RestActions.hpp"

using tcp = boost::asio::ip::tcp;
namespace beast = boost::beast;
namespace http = beast::http;

// (Moved) Request/response structs and registration live in RestActions.cpp.

// Singleton accessor for the action registry, with lazy registration.
static Slic3r::Utils::ActionRegister& rest_actions()
{
	static Slic3r::Utils::ActionRegister reg;
	static bool initialized = false;
	if (!initialized) {
		register_rest_actions(reg);
		initialized = true;
	}
	return reg;
}

RestServer::RestServer(const std::string& address, unsigned short port)
	: m_address(address), m_port(port)
{
	m_running = true;
	m_thread = std::thread([this]{ run(); });
}

RestServer::~RestServer()
{
	stop();
}

void RestServer::stop()
{
	if (!m_running)
		return;
	m_running = false;
	// creating a connection to ourselves will unblock accept
	try {
		boost::asio::io_context ioc;
		tcp::socket sock(ioc);
		sock.connect(tcp::endpoint(boost::asio::ip::make_address(m_address), m_port));
		sock.close();
	} catch (...) {
		// ignore
	}
	if (m_thread.joinable())
		m_thread.join();
}

void RestServer::run()
{
	try {
		boost::asio::io_context ioc{1};

		tcp::acceptor acceptor{ioc, {boost::asio::ip::make_address(m_address), m_port}};

		while (m_running) {
			beast::error_code ec;
			tcp::socket socket{ioc};
			acceptor.accept(socket, ec);
			if (ec) {
				if (!m_running)
					break;
				BOOST_LOG_TRIVIAL(error) << "RestServer accept error: " << ec.message();
				continue;
			}

			// Handle the connection synchronously on this thread.
			beast::flat_buffer buffer;
			http::request<http::string_body> req;
			http::read(socket, buffer, req, ec);
			if (ec) {
				BOOST_LOG_TRIVIAL(error) << "RestServer read error: " << ec.message();
				continue;
			}

			http::response<http::string_body> res{http::status::ok, req.version()};
			res.set(http::field::server, "OrcaSlicer-REST/0");
			res.keep_alive(req.keep_alive());

			// Log method and target for debugging
			try {
				auto ms = req.method_string();
				std::string method_str(ms.data(), ms.size());
				auto ts = req.target();
				std::string target_str(ts.data(), ts.size());
				BOOST_LOG_TRIVIAL(info) << "[RestServer] Request: method=" << method_str << " target=" << target_str;
			} catch (...) {}

			if (req.method() == http::verb::post && (req.target() == "/rpc" || req.target() == "/rpc/")) {
				std::string body = req.body();
				BOOST_LOG_TRIVIAL(info) << "[RestServer] RPC invoked, body size=" << body.size();

				std::string resp_status = "error";
				std::string resp_msg;
				bool handled_by_registry = false;
				std::string registry_response;

				try {
					// Directly parse JSON; parsing errors are handled by the catch block
					nlohmann::json j = nlohmann::json::parse(body);
					std::string action = j.value("action", std::string());

					if (!action.empty()) {
						auto &reg = rest_actions();
						std::string action_result;
						std::string task_id = Slic3r::Utils::start_task_for_action_and_run(reg, action, body, action_result);
						if (!task_id.empty()) {
							// Merge task_id into response JSON
							try {
								nlohmann::json jr = nlohmann::json::parse(action_result);
								jr["task_id"] = task_id;
								registry_response = jr.dump();
							} catch (...) {
								// Fall back to wrapping as object
								nlohmann::json jr; jr["result"] = action_result; jr["task_id"] = task_id; registry_response = jr.dump();
							}
							handled_by_registry = true;
							BOOST_LOG_TRIVIAL(info) << "[RestServer] RPC handled by ActionRegister: action='" << action << "' task_id=" << task_id;
						} else {
							resp_msg = std::string("unknown action: ") + action;
						}
					} else {
						resp_msg = "missing action";
					}
				} catch (const nlohmann::json::parse_error &jex) {
					resp_msg = std::string("json parse error: ") + jex.what();
				} catch (const std::exception &ex) {
					resp_msg = std::string("exception: ") + ex.what();
				}

				if (handled_by_registry) {
					res.set(http::field::content_type, "application/json");
					res.body() = registry_response;
					res.prepare_payload();
				} else {
					// Build JSON response using nlohmann
					nlohmann::json r;
					r["status"]  = resp_status;
					r["message"] = resp_msg;
					res.set(http::field::content_type, "application/json");
					res.body() = r.dump();
					res.prepare_payload();
				}
			} else {
				res.result(http::status::not_found);
				res.body() = "Not found";
				res.prepare_payload();
			}

			http::write(socket, res, ec);
			socket.shutdown(tcp::socket::shutdown_send, ec);
		}
	} catch (std::exception& ex) {
		BOOST_LOG_TRIVIAL(error) << "RestServer exception: " << ex.what();
	}
}

// Global server pointer controlled by the main application via the helpers
// below. This avoids starting the server during static initialization.
static std::unique_ptr<RestServer> g_rest_server;

void start_rest_server(const char* address, unsigned short port)
{
	if (!g_rest_server) {
		try {
			g_rest_server.reset(new RestServer(address ? address : "127.0.0.1", port));
			BOOST_LOG_TRIVIAL(info) << "[RestServer] started on " << (address ? address : "127.0.0.1") << ":" << port;
		} catch (const std::exception &ex) {
			BOOST_LOG_TRIVIAL(error) << "[RestServer] failed to start: " << ex.what();
		}
	}
}

void stop_rest_server()
{
	if (g_rest_server) {
		try {
			g_rest_server->stop();
		} catch (...) {}
		g_rest_server.reset();
		BOOST_LOG_TRIVIAL(info) << "[RestServer] stopped";
	}
}
