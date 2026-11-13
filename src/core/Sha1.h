#pragma once

#include <QString>

namespace md {

// 计算文件 SHA1，返回小写十六进制串；失败返回空串并填充 *err。
// 分块读取，内存占用与文件大小无关。
QString sha1File(const QString &path, QString *err = nullptr);

// 校验一个已存在的文件是否符合期望的 SHA1（空 sha1 视为「不校验」）。
// 不匹配时应删除该文件再重下。
bool verifyFile(const QString &path, const QString &sha1, qint64 expectedSize,
                QString *err = nullptr);

} // namespace md
