// SPDX-License-Identifier: GPL-3.0-or-later
#ifndef SEAMLY_MCP_MEASUREMENTS_H
#define SEAMLY_MCP_MEASUREMENTS_H
#include <QByteArray>
#include <QJsonObject>
#include <QString>

namespace McpMeasurements
{
/** @brief A validated measurement document and its calculated metadata. */
struct Table
{
    QByteArray xml;
    QJsonObject info;
};

/**
 * @brief Identify a measurement file by its absolute path and complete contents.
 * @param path Measurement file location.
 * @param bytes Current file contents.
 * @return SHA-256 token used to reject stale file writes.
 */
QString token(const QString &path, const QByteArray &bytes);

/**
 * @brief Read a measurement file subject to the 4 MiB document limit.
 * @param path File to open without modifying it.
 * @return The complete file contents.
 * @throws std::runtime_error If the file cannot be read or exceeds the size limit.
 */
QByteArray readBytes(const QString &path);

/**
 * @brief Convert and evaluate measurement XML using a private temporary copy.
 * @param bytes Individual or multisize measurement XML.
 * @param size Evaluation size in file units; a negative value selects the base size.
 * @param height Evaluation height in file units; a negative value selects the base height.
 * @return Validated XML and calculated entries without changing the source file.
 * @throws std::runtime_error If the format, names or formulas are invalid.
 */
Table inspect(const QByteArray &bytes, double size = -1, double height = -1);

/**
 * @brief Prepare and validate a measurement-file update without publishing it.
 * @param original Existing XML, or empty bytes when creating a new file.
 * @param arguments File type, unit, entry updates and removals.
 * @return The validated candidate document and calculated metadata.
 * @throws std::runtime_error If an update violates file or measurement constraints.
 */
Table prepare(const QByteArray &original, const QJsonObject &arguments);
} // namespace McpMeasurements
#endif
