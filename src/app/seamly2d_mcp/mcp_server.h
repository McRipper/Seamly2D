// SPDX-License-Identifier: GPL-3.0-or-later
#ifndef SEAMLY_MCP_SERVER_H
#define SEAMLY_MCP_SERVER_H

#include <QFile>
#include <QJsonArray>
#include <QJsonObject>
#include <QJsonValue>
#include <QLocalSocket>
#include <functional>
#include <memory>

/** @brief Translate MCP requests into calls to the explicitly enabled local GUI bridge. */
class McpServer
{
public:
    using BridgeCall = std::function<QJsonObject(const QString &, const QString &, const QJsonObject &)>;

    /**
     * @brief Create an MCP session without starting or changing the GUI.
     * @param socket_path Local endpoint used by the running Seamly2D instance.
     * @param bridge Request transport; tests may supply an in-process substitute.
     */
    explicit McpServer(const QString &socket_path, BridgeCall bridge = callBridge);

    /**
     * @brief Validate and dispatch a single JSON-RPC request or notification.
     * @param request Parsed JSON value received from the client.
     * @return A response object, or Undefined for notifications that require no reply.
     */
    QJsonValue handle(const QJsonValue &request);

    /** @brief Keep an initialized client's presence connection open, excluding GUI probes. */
    void trackConnection();

    /**
     * @brief Construct a JSON-RPC error response.
     * @param id Request identifier; Undefined is represented as null.
     * @param code JSON-RPC or MCP error code.
     * @param message Human-readable error description.
     * @return A complete JSON-RPC error object.
     */
    static QJsonObject error(const QJsonValue &id, int code, const QString &message);

    /**
     * @brief Send one bounded request to the GUI without retrying it.
     * @param socket_path Local endpoint used by the running Seamly2D instance.
     * @param method Bridge operation to invoke.
     * @param arguments Validated operation arguments.
     * @return The GUI's structured result.
     * @throws std::runtime_error If transport or application validation fails.
     */
    static QJsonObject callBridge(const QString &socket_path, const QString &method, const QJsonObject &arguments);

    /** @brief Return the bundled tool catalog, including schemas and annotations. */
    static QJsonArray tools();

private:
    enum class Phase
    {
        New,
        Initializing,
        Ready
    };
    QString m_socket_path;
    BridgeCall m_bridge;
    Phase m_phase = Phase::New;
    QString m_protocol = QStringLiteral("2025-06-18");
    QJsonObject m_legacy_capabilities;
    bool m_modern_client_ready = false;
    std::unique_ptr<QLocalSocket> m_presence;
};

/**
 * @brief Serve bounded, newline-delimited UTF-8 requests over stdio.
 * @param server Session that validates and handles incoming requests.
 * @param input Open input stream; an empty read ends the transport.
 * @param output Open output stream reserved for protocol responses.
 * @return Zero on orderly closure, or one on an input/output failure.
 */
int serveMcp(McpServer &server, QFile &input, QFile &output);

#endif
