#include "core/RulesEval.h"

#include <QJsonObject>
#include <QRegularExpression>
#include <QSysInfo>

namespace md {

OsInfo OsInfo::current()
{
    OsInfo os;
    os.name = QStringLiteral("windows");
#if defined(Q_PROCESSOR_X86_64) || defined(__x86_64__)
    os.arch = QStringLiteral("x86_64");
#elif defined(Q_PROCESSOR_ARM_64)
    os.arch = QStringLiteral("arm64");
#else
    os.arch = QStringLiteral("x86");
#endif
    os.version = QSysInfo::kernelVersion();
    return os;
}

namespace {

// 单条规则是否适用于当前环境。带 features 的规则（demo / custom_resolution）
// 本程序不涉及，一律视为不匹配。
bool ruleMatches(const QJsonObject &rule, const OsInfo &os)
{
    if (rule.contains(QStringLiteral("features")))
        return false;

    const QJsonValue osVal = rule.value(QStringLiteral("os"));
    if (osVal.isObject()) {
        const QJsonObject o = osVal.toObject();
        const QString wantName = o.value(QStringLiteral("name")).toString();
        if (!wantName.isEmpty() && wantName != os.name)
            return false;

        const QString wantArch = o.value(QStringLiteral("arch")).toString();
        if (!wantArch.isEmpty()) {
            // 规则里 32 位写 "x86"，64 位写 "x86_64"/"amd64"
            const bool is32Wanted = (wantArch == QStringLiteral("x86"));
            const bool is32Have = (os.arch == QStringLiteral("x86"));
            if (is32Wanted != is32Have)
                return false;
            const bool archOk = (wantArch == os.arch)
                                || (wantArch == QStringLiteral("amd64")
                                    && os.arch == QStringLiteral("x86_64"));
            if (!archOk)
                return false;
        }

        const QString wantVersion = o.value(QStringLiteral("version")).toString();
        if (!wantVersion.isEmpty()) {
            QRegularExpression re(wantVersion);
            if (re.isValid() && !re.match(os.version).hasMatch())
                return false;
        }
    }
    return true;
}

} // namespace

bool rulesAllow(const QJsonArray &rules, const OsInfo &os)
{
    if (rules.isEmpty())
        return true;

    bool allowed = false;
    for (const QJsonValue &v : rules) {
        if (!v.isObject())
            continue;
        const QJsonObject rule = v.toObject();
        if (!ruleMatches(rule, os))
            continue;
        const QString action = rule.value(QStringLiteral("action")).toString();
        allowed = (action == QStringLiteral("allow"));
    }
    return allowed;
}

} // namespace md
