#include "features/tiling/tiling_rules.h"

#include <QSet>
#include <QVariantMap>

namespace waku::tiling {

ParsedRules parseRules(const QVariantList& entries) {
  ParsedRules out;
  int index = 0;
  for (const QVariant& entry : entries) {
    const QString where = QStringLiteral("rule %1: ").arg(index++);
    const QVariantMap map = entry.toMap();
    Rule rule;
    rule.process = map.value(QStringLiteral("process")).toString().trimmed();
    rule.className = map.value(QStringLiteral("class")).toString().trimmed();
    if (rule.className.isEmpty()) {
      rule.className = map.value(QStringLiteral("className")).toString().trimmed();
    }
    const QString title = map.value(QStringLiteral("title")).toString();
    if (rule.process.isEmpty() && rule.className.isEmpty() && title.isEmpty()) {
      out.errors << where + QStringLiteral("sets none of process, class or title");
      continue;
    }
    if (!title.isEmpty()) {
      rule.title = QRegularExpression(title, QRegularExpression::CaseInsensitiveOption);
      if (!rule.title.isValid()) {
        out.errors << where + QStringLiteral("title is not a valid regular expression: ") +
                          rule.title.errorString();
        continue;
      }
    }
    const QString action = map.value(QStringLiteral("action"), QStringLiteral("ignore"))
                               .toString()
                               .trimmed()
                               .toLower();
    if (action == QLatin1String("ignore")) {
      rule.action = RuleAction::Ignore;
    } else if (action == QLatin1String("float")) {
      rule.action = RuleAction::Float;
    } else if (action == QLatin1String("tile")) {
      rule.action = RuleAction::Tile;
    } else {
      out.errors
          << where + QStringLiteral("unknown action '%1' (ignore, float or tile)").arg(action);
      continue;
    }
    out.rules.push_back(std::move(rule));
  }
  return out;
}

std::optional<RuleAction> matchRules(const std::vector<Rule>& rules, const WindowFacts& facts) {
  for (const Rule& rule : rules) {
    if (!rule.process.isEmpty() &&
        rule.process.compare(facts.processName, Qt::CaseInsensitive) != 0) {
      continue;
    }
    if (!rule.className.isEmpty() &&
        rule.className.compare(facts.className, Qt::CaseInsensitive) != 0) {
      continue;
    }
    if (rule.title.isValid() && !rule.title.pattern().isEmpty() &&
        !rule.title.match(facts.title).hasMatch()) {
      continue;
    }
    return rule.action;
  }
  return std::nullopt;
}

bool isBuiltinIgnoredClass(const QString& className) {
  static const QSet<QString> classes = {
      QStringLiteral("msosplash"),
      QStringLiteral("splashscreen"),
      QStringLiteral("splash"),
      QStringLiteral("progman"),
      QStringLiteral("workerw"),
      QStringLiteral("shell_traywnd"),
      QStringLiteral("shell_secondarytraywnd"),
      QStringLiteral("foregroundstaging"),
      QStringLiteral("applicationmanager_desktopshellwindow"),
      QStringLiteral("windows.ui.core.corewindow"),
      QStringLiteral("xamlexplorerhostislandwindow"),
      QStringLiteral("toplevelwindowforoverflowxamlisland"),
      QStringLiteral("tooltips_class32"),
      QStringLiteral("#32768"),
  };
  return classes.contains(className.toLower());
}

Eligibility classify(const WindowFacts& facts, const std::vector<Rule>& rules, bool selfElevated,
                     bool forceTile) {
  auto ignore = [](const char* reason, bool lasting = false) {
    return Eligibility{Verdict::Ignore, QString::fromLatin1(reason), lasting};
  };
  if (facts.ownProcess) return ignore("own process", true);
  if (!facts.visible) return ignore("not visible");
  if (facts.cloaked) return ignore("cloaked");
  if (facts.iconic) return {Verdict::Retry, QStringLiteral("minimized")};
  if (facts.title.isEmpty()) return {Verdict::Retry, QStringLiteral("untitled")};
  if (facts.ownedByVisible) return ignore("owned by a visible window");
  if (facts.toolWindow) return ignore("tool window", true);
  // A `tile` rule opts a window in past the two frame checks: a game in windowed mode often has a
  // fixed-size or frameless window, which SetWindowPos still moves and sizes. The rules are only
  // consulted for a window that fails a check, so an ordinary window costs nothing extra.
  if ((!facts.thickFrame || !facts.caption) && matchRules(rules, facts) != RuleAction::Tile) {
    return ignore(facts.thickFrame ? "no caption" : "not resizable", true);
  }
  if (facts.zoomed) return ignore("maximized");
  if (facts.fullscreen) return ignore("fullscreen");
  if (isBuiltinIgnoredClass(facts.className)) return ignore("ignored class", true);
  if (facts.elevated && !selfElevated) return ignore("elevated", true);
  if (!forceTile) {
    const std::optional<RuleAction> action = matchRules(rules, facts);
    if (action == RuleAction::Ignore) return ignore("rule");
    if (action == RuleAction::Float) return {Verdict::Float, QStringLiteral("rule")};
  }
  return {Verdict::Manage, QString()};
}

}  // namespace waku::tiling
