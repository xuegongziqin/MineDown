#include "core/JavaLocator.h"

#include "core/Settings.h"

#include <QDir>
#include <QFileInfo>
#include <QProcess>
#include <QRegularExpression>
#include <QSet>
#include <QStandardPaths>

#include <algorithm>

namespace md {
namespace {

QStringList javaCandidates()
{
    QStringList list;

    const QString configured = Settings::instance().javaPath();
    if (!configured.isEmpty())
        list.append(QDir::fromNativeSeparators(configured));

    const QString onPath = QStandardPaths::findExecutable(QStringLiteral("java"));
    if (!onPath.isEmpty())
        list.append(onPath);

    QStringList roots = {
        QStringLiteral("C:/Program Files/Java"),
        QStringLiteral("C:/Program Files/Eclipse Adoptium"),
        QStringLiteral("C:/Program Files/Microsoft"),
        QStringLiteral("C:/Program Files/Zulu"),
        QStringLiteral("C:/Program Files/BellSoft"),
        QStringLiteral("C:/Program Files/Amazon Corretto"),
        QStringLiteral("C:/Program Files/IBM/Java"),
    };
    const QString localApp = qEnvironmentVariable("LOCALAPPDATA");
    if (!localApp.isEmpty())
        roots.append(localApp + QStringLiteral("/Programs/Eclipse Adoptium"));
    const QString userProfile = qEnvironmentVariable("USERPROFILE");
    if (!userProfile.isEmpty())
        roots.append(userProfile + QStringLiteral("/.jdks"));

    for (const QString &root : roots) {
        const QDir dir(root);
        if (!dir.exists())
            continue;
        const QStringList subs = dir.entryList(QDir::Dirs | QDir::NoDotAndDotDot);
        for (const QString &sub : subs) {
            const QString exe = dir.filePath(sub + QStringLiteral("/bin/java.exe"));
            if (QFileInfo::exists(exe))
                list.append(exe);
            const QString exe2 = dir.filePath(sub + QStringLiteral("/bin/java"));
            if (QFileInfo::exists(exe2))
                list.append(exe2);
        }
    }
    return list;
}

// 从 `java -version` 的输出里抠出主版本号。
int parseMajor(const QString &text)
{
    static const QRegularExpression re(QStringLiteral("\"([0-9][^\"]*)\""));
    const auto m = re.match(text);
    if (!m.hasMatch())
        return -1;
    const QString ver = m.captured(1);
    const QStringList parts = ver.split(u'.');
    if (parts.size() >= 2 && parts.at(0) == QStringLiteral("1")) {
        bool ok = false;
        const int major = parts.at(1).toInt(&ok);
        return ok ? major : -1;
    }
    bool ok = false;
    const int major = parts.at(0).toInt(&ok);
    return ok ? major : -1;
}

JavaInfo probe(const QString &exe)
{
    JavaInfo info;
    const QFileInfo fi(exe);
    if (!fi.exists() || !fi.isFile())
        return info;

    QProcess proc;
    proc.setProcessChannelMode(QProcess::MergedChannels);
    proc.start(exe, {QStringLiteral("-version")});
    if (!proc.waitForFinished(8000))
        return info;

    const QString out = QString::fromLocal8Bit(proc.readAll());
    const int major = parseMajor(out);
    if (major <= 0)
        return info;

    static const QRegularExpression re(QStringLiteral("\"([0-9][^\"]*)\""));
    const auto m = re.match(out);

    info.path = fi.canonicalFilePath();
    info.major = major;
    info.version = m.hasMatch() ? m.captured(1) : QString::number(major);
    return info;
}

QVector<JavaInfo> &cache()
{
    static QVector<JavaInfo> list;
    return list;
}

} // namespace

QVector<JavaInfo> scanJavaRuntimes(bool refresh)
{
    if (!refresh && !cache().isEmpty())
        return cache();

    QVector<JavaInfo> result;
    QSet<QString> seen;
    for (const QString &exe : javaCandidates()) {
        const QString key = QFileInfo(exe).canonicalFilePath();
        if (key.isEmpty() || seen.contains(key))
            continue;
        seen.insert(key);
        const JavaInfo info = probe(exe);
        if (info.isValid())
            result.append(info);
    }

    std::sort(result.begin(), result.end(), [](const JavaInfo &a, const JavaInfo &b) {
        if (a.major != b.major)
            return a.major > b.major;
        return a.path.size() < b.path.size();
    });

    cache() = result;
    return result;
}

JavaInfo findJava(int minMajor)
{
    const QVector<JavaInfo> list = scanJavaRuntimes();
    for (const JavaInfo &info : list) {
        if (info.major >= minMajor)
            return info;
    }
    if (!list.isEmpty())
        return list.first();
    return {};
}

QString javaVersionText(const JavaInfo &info)
{
    if (!info.isValid())
        return QStringLiteral("未找到 Java");
    return QStringLiteral("Java %1（%2）").arg(info.major).arg(info.version);
}

} // namespace md
