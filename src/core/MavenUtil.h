#pragma once

#include <QString>
#include <QStringList>

namespace md {

// Maven 坐标的解析与路径换算。
//
// 加载器的安装器里到处是这种写法：
//   install_profile.json 的 data 值： "[de.oceanlabs.mcp:mcp_config:1.20.1-20230612.114412:mappings@txt]"
//   processors 的 classpath：        "net.minecraftforge:installertools:1.4.1"
//   processors 的 args 里还会直接出现："--input [de.oceanlabs.mcp:mcp_config:...@zip]"
// 所以统一用这个结构解析：group:artifact:version[:classifier][@ext]。
struct MavenCoord {
    QString group;
    QString artifact;
    QString version;
    QString classifier;
    QString ext = QStringLiteral("jar");

    bool isValid() const
    {
        return !group.isEmpty() && !artifact.isEmpty() && !version.isEmpty();
    }

    // group/as/path/artifact/version/artifact-version[-classifier].ext
    QString relPath() const
    {
        QString g = group;
        g.replace(u'.', u'/');
        QString name = artifact + u'-' + version;
        if (!classifier.isEmpty())
            name += u'-' + classifier;
        return g + u'/' + artifact + u'/' + version + u'/' + name + u'.' + ext;
    }

    QString coordinate() const
    {
        QString s = group + u':' + artifact + u':' + version;
        if (!classifier.isEmpty())
            s += u':' + classifier;
        if (ext != QStringLiteral("jar"))
            s += u'@' + ext;
        return s;
    }
};

// 解析 "group:artifact:version[:classifier][@ext]"，允许外面包一层方括号。
inline bool parseMavenCoord(const QString &text, MavenCoord *out)
{
    if (!out)
        return false;

    QString s = text.trimmed();
    if (s.startsWith(u'[') && s.endsWith(u']'))
        s = s.mid(1, s.size() - 2).trimmed();
    if (s.isEmpty())
        return false;

    MavenCoord c;
    const int at = s.lastIndexOf(u'@');
    if (at > 0) {
        c.ext = s.mid(at + 1).trimmed();
        s = s.left(at);
    }
    if (c.ext.isEmpty())
        c.ext = QStringLiteral("jar");

    const QStringList parts = s.split(u':');
    if (parts.size() < 3 || parts.size() > 4)
        return false;

    c.group = parts.at(0).trimmed();
    c.artifact = parts.at(1).trimmed();
    c.version = parts.at(2).trimmed();
    if (parts.size() == 4)
        c.classifier = parts.at(3).trimmed();

    if (!c.isValid())
        return false;

    *out = c;
    return true;
}

// 形如 "[g:a:v@ext]"：只有整串被方括号包住才算（processor 参数里的坐标都这样写）。
inline bool isBracketCoord(const QString &text)
{
    const QString s = text.trimmed();
    return s.size() > 2 && s.startsWith(u'[') && s.endsWith(u']');
}

// <root>/libraries/<relPath>
inline QString mavenLocalPath(const QString &root, const MavenCoord &c)
{
    return root + QStringLiteral("/libraries/") + c.relPath();
}

// <repo>/<relPath>；repo 不带结尾斜杠。
inline QString mavenRemoteUrl(const QString &repo, const MavenCoord &c)
{
    QString base = repo;
    while (base.endsWith(u'/'))
        base.chop(1);
    return base + u'/' + c.relPath();
}

} // namespace md
