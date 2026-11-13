#include "core/Settings.h"

#include "core/Mirror.h"

namespace md {

Settings &Settings::instance()
{
    static Settings inst;
    return inst;
}

QVariant Settings::get(const QString &key, const QVariant &fallback) const
{
    if (m_transient) {
        const auto it = m_overrides.constFind(key);
        if (it != m_overrides.constEnd())
            return it.value();
    }
    return s_.value(key, fallback);
}

void Settings::put(const QString &key, const QVariant &value)
{
    if (m_transient) {
        m_overrides.insert(key, value);
        return;
    }
    s_.setValue(key, value);
    s_.sync();
}

void Settings::setTransientOverrides(bool on)
{
    m_transient = on;
    if (!on)
        m_overrides.clear();
}

QString Settings::rootDir() const
{
    return get(QStringLiteral("game/rootDir"), QString()).toString();
}

void Settings::setRootDir(const QString &dir)
{
    put(QStringLiteral("game/rootDir"), dir);
}

bool Settings::hasRootDir() const
{
    const QString d = rootDir();
    return !d.isEmpty();
}

QString Settings::mirrorKey() const
{
    const QString key = get(QStringLiteral("net/mirror"), QStringLiteral("bmclapi")).toString();
    if (!findMirror(key))
        return QStringLiteral("bmclapi");
    return key;
}

void Settings::setMirrorKey(const QString &key)
{
    put(QStringLiteral("net/mirror"), key);
}

QString Settings::customMirrorBase() const
{
    return get(QStringLiteral("net/customBase"), QString()).toString();
}

void Settings::setCustomMirrorBase(const QString &base)
{
    put(QStringLiteral("net/customBase"), base);
}

QString Settings::mirrorBase() const
{
    const QString key = mirrorKey();
    if (key == QStringLiteral("official"))
        return QString();
    if (key == QStringLiteral("custom")) {
        QString base = customMirrorBase().trimmed();
        while (base.endsWith(u'/'))
            base.chop(1);
        return base;
    }
    const MirrorInfo *m = findMirror(key);
    return m ? m->base : QString();
}

int Settings::concurrency() const
{
    // 默认 8：实测 BMCLAPI 对同一 IP 的并发很敏感，16 路经常被 429 限流；
    // 大文件还有分段并行兜底，8 路已经够快。
    int n = get(QStringLiteral("net/concurrency"), 8).toInt();
    if (n < 1) n = 1;
    if (n > 32) n = 32;
    return n;
}

void Settings::setConcurrency(int n)
{
    if (n < 1) n = 1;
    if (n > 32) n = 32;
    put(QStringLiteral("net/concurrency"), n);
}

QString Settings::lastVersion() const
{
    return get(QStringLiteral("ui/lastVersion"), QString()).toString();
}

void Settings::setLastVersion(const QString &id)
{
    put(QStringLiteral("ui/lastVersion"), id);
}

bool Settings::showSnapshots() const
{
    return get(QStringLiteral("ui/showSnapshots"), true).toBool();
}

void Settings::setShowSnapshots(bool on)
{
    put(QStringLiteral("ui/showSnapshots"), on);
}

QString Settings::javaPath() const
{
    return get(QStringLiteral("game/javaPath"), QString()).toString();
}

void Settings::setJavaPath(const QString &path)
{
    put(QStringLiteral("game/javaPath"), path);
}

void Settings::sync()
{
    if (!m_transient)
        s_.sync();
}

} // namespace md
