#include "features/tiling/qml/tiling_service.h"

#include <QDebug>
#include <QGuiApplication>
#include <QJSEngine>
#include <QQmlEngine>
#include <QScreen>
#include <algorithm>
#include <cmath>
#include <exception>
#include <utility>

#include "features/tiling/layouts.h"
#include "features/tiling/tiling_rules.h"
#include "kernel/qt/live_desktop.h"

namespace waku::tiling {
namespace {

Tiling::Deps liveDeps() {
  Tiling::Deps deps;
  if (!kernel::liveDesktop(QStringLiteral("tiling"))) return deps;
  deps.placer = makeWin32WindowPlacer();
  deps.hooks = makeWin32TilingHooks();
  deps.scheduler = makeQtScheduler();
  deps.selfElevated = processIsElevated();
  return deps;
}

QString yesNo(bool value) { return value ? QStringLiteral("yes") : QStringLiteral("no"); }

QString dropKindText(DropKind kind) {
  switch (kind) {
    case DropKind::Swap:
      return QStringLiteral("swap");
    case DropKind::Stack:
      return QStringLiteral("stack");
    case DropKind::Monitor:
      return QStringLiteral("monitor");
    case DropKind::Back:
      return QStringLiteral("back");
    case DropKind::Float:
      return QStringLiteral("float");
    case DropKind::None:
      break;
  }
  return QString();
}

QVariantMap inactiveResizePreview() {
  return {{QStringLiteral("active"), false},
          {QStringLiteral("screen"), QString()},
          {QStringLiteral("rects"), QVariantList()}};
}

// The screen in physical pixels. QScreen::geometry() on Windows keeps the physical origin with the
// logical size, as in host::physicalScreenRect.
QRect physicalGeometry(const QScreen* screen) {
  const QRect geometry = screen->geometry();
  const double dpr = screen->devicePixelRatio() > 0 ? screen->devicePixelRatio() : 1.0;
  return QRect(geometry.topLeft(), QSize(static_cast<int>(std::lround(geometry.width() * dpr)),
                                         static_cast<int>(std::lround(geometry.height() * dpr))));
}

// The screen whose physical rectangle holds `point` (physical pixels), or null.
QScreen* screenHolding(const QPoint& point) {
  if (!qGuiApp) return nullptr;
  for (QScreen* screen : QGuiApplication::screens()) {
    if (physicalGeometry(screen).contains(point)) return screen;
  }
  return nullptr;
}

// `rect` (physical pixels) in the logical pixels of `screen`, from its top-left corner.
QRect logicalOn(const QScreen* screen, const QRect& rect) {
  const QRect geometry = screen->geometry();
  const double dpr = screen->devicePixelRatio() > 0 ? screen->devicePixelRatio() : 1.0;
  const auto logical = [dpr](int value) { return static_cast<int>(std::lround(value / dpr)); };
  return QRect(logical(rect.x() - geometry.x()), logical(rect.y() - geometry.y()),
               logical(rect.width()), logical(rect.height()));
}

QVariantMap inactiveDropTarget() {
  return {{QStringLiteral("active"), false},
          {QStringLiteral("kind"), QString()},
          {QStringLiteral("screen"), QString()},
          {QStringLiteral("x"), 0},
          {QStringLiteral("y"), 0},
          {QStringLiteral("width"), 0},
          {QStringLiteral("height"), 0}};
}

}  // namespace

Tiling::Tiling(QObject* parent) : Tiling(liveDeps(), parent) {}

Tiling::Tiling(Deps deps, QObject* parent) : QObject(parent) { init(std::move(deps)); }

Tiling::~Tiling() {
  // The windows stay where they are; only the hooks go.
  if (hooks_) hooks_->uninstall();
}

void Tiling::init(Deps deps) {
  placer_ = std::move(deps.placer);
  hooks_ = std::move(deps.hooks);
  scheduler_ = std::move(deps.scheduler);
  settings_.selfElevated = deps.selfElevated;
  watchScreens_ = deps.watchScreens;
  if (placer_ && scheduler_) engine_ = std::make_unique<TilingEngine>(*placer_, *scheduler_);
  dropTarget_ = inactiveDropTarget();
  resizePreview_ = inactiveResizePreview();
  if (engine_) {
    engine_->setDropListener([this](const DropTarget& target) { updateDropTarget(target); });
    engine_->setPreviewListener(
        [this](const ResizePreview& preview) { updateResizePreview(preview); });
    // Another desktop shows another workspace, with a layout of its own.
    engine_->setDesktopListener([this] { refreshCurrentLayout(); });
  }

  screenTimer_.setSingleShot(true);
  screenTimer_.setInterval(deps.screenDebounceMs);
  connect(&screenTimer_, &QTimer::timeout, this, [this] {
    if (active_ && engine_) engine_->retileAll();
  });
  if (watchScreens_ && qGuiApp) {
    connect(qGuiApp, &QGuiApplication::screenAdded, this, [this](QScreen* screen) {
      watchScreen(screen);
      screensChanged();
    });
    connect(qGuiApp, &QGuiApplication::screenRemoved, this, [this](QScreen*) { screensChanged(); });
    for (QScreen* screen : QGuiApplication::screens()) watchScreen(screen);
  }
}

Tiling* Tiling::create(QQmlEngine*, QJSEngine*) {
  // The engine's tiler is on unless the run forbids it (--no-tiling) or the shell sets
  // `enabled = false`. A Tiling built directly (tests) starts off.
  auto* tiling = new Tiling(nullptr);
  tiling->setEnabled(true);
  return tiling;
}

void Tiling::watchScreen(QScreen* screen) {
  connect(screen, &QScreen::availableGeometryChanged, this, &Tiling::screensChanged);
  connect(screen, &QScreen::geometryChanged, this, &Tiling::screensChanged);
}

void Tiling::screensChanged() {
  if (active_) screenTimer_.start();
}

// --- switch

void Tiling::reevaluate() {
  try {
    const bool want = enabled_ && engine_ && hooks_;
    if (want == active_) return;
    if (want) {
      if (!hooks_->install([this](const WindowEvent& event) { onEvent(event); })) {
        qWarning() << "tiling: no window event hook could be installed";
        return;
      }
      pushSettings();
      engine_->start();
      active_ = true;
    } else {
      hooks_->uninstall();
      engine_->stop();
      active_ = false;
    }
    emit activeChanged();
    refreshCurrentLayout();
  } catch (const std::exception& e) {
    qWarning().noquote() << "tiling: reevaluate failed:" << e.what();
  }
}

void Tiling::onEvent(const WindowEvent& event) {
  try {
    if (hooks_ && (event.kind == EventKind::MoveSizeEnd || event.kind == EventKind::Destroy)) {
      hooks_->trackLocation(event.hwnd, false);
    }
    if (engine_) engine_->handle(event);
    if (hooks_ && event.kind == EventKind::MoveSizeStart) {
      hooks_->trackLocation(event.hwnd, engine_ && engine_->tracksLocation(event.hwnd));
    }
    if (event.kind == EventKind::Foreground) refreshCurrentLayout();
  } catch (const std::exception& e) {
    qWarning().noquote() << "tiling: event failed:" << e.what();
  } catch (...) {
    qWarning() << "tiling: event failed";
  }
}

// --- settings

void Tiling::setEnabled(bool enabled) {
  if (enabled == enabled_) return;
  enabled_ = enabled;
  emit enabledChanged();
  reevaluate();
}

void Tiling::pushSettings() {
  if (engine_) engine_->setSettings(settings_);
  refreshCurrentLayout();
}

void Tiling::refreshCurrentLayout() {
  const QString now = active_ && engine_ ? engine_->currentLayout() : QString();
  if (now == currentLayout_) return;
  currentLayout_ = now;
  emit currentLayoutChanged();
}

void Tiling::setLayoutName(const QString& name) {
  const QString layout = canonicalLayout(name);
  if (layout.isEmpty()) {
    qWarning().noquote() << "tiling: unknown layout" << name << "("
                         << layouts().join(QLatin1String(", ")) << ")";
    return;
  }
  if (layout == settings_.layout) return;
  settings_.layout = layout;
  pushSettings();
  emit layoutChanged();
}

void Tiling::setGap(int gap) {
  gap = std::clamp(gap, 0, 200);
  if (gap == settings_.gap) return;
  settings_.gap = gap;
  pushSettings();
  emit gapChanged();
}

void Tiling::setRatio(double ratio) {
  ratio = std::clamp(ratio, 0.1, 0.9);
  if (qFuzzyCompare(ratio, settings_.ratio)) return;
  settings_.ratio = ratio;
  pushSettings();
  emit ratioChanged();
}

void Tiling::setRules(const QVariantList& rules) {
  rules_ = rules;
  const ParsedRules parsed = parseRules(rules);
  for (const QString& error : parsed.errors) qWarning().noquote() << "tiling:" << error;
  settings_.rules = parsed.rules;
  pushSettings();
  emit rulesChanged();
}

QStringList Tiling::layouts() const { return layoutNames(); }

QString Tiling::dragAction() const {
  return settings_.dragAction == DragAction::Swap ? QStringLiteral("swap")
                                                  : QStringLiteral("float");
}

void Tiling::setDragAction(const QString& action) {
  const QString name = action.trimmed().toLower();
  DragAction value = DragAction::Float;
  if (name == QLatin1String("swap")) {
    value = DragAction::Swap;
  } else if (name != QLatin1String("float")) {
    qWarning().noquote() << "tiling: unknown dragAction" << action << "(float, swap)";
    return;
  }
  if (value == settings_.dragAction) return;
  settings_.dragAction = value;
  pushSettings();
  emit dragActionChanged();
}

QString Tiling::dragFloatModifier() const {
  switch (settings_.dragFloatKey) {
    case ModifierKey::Alt:
      return QStringLiteral("alt");
    case ModifierKey::Shift:
      return QStringLiteral("shift");
    case ModifierKey::Ctrl:
      break;
  }
  return QStringLiteral("ctrl");
}

void Tiling::setDragFloatModifier(const QString& key) {
  const QString name = key.trimmed().toLower();
  ModifierKey value = ModifierKey::Ctrl;
  if (name == QLatin1String("alt")) {
    value = ModifierKey::Alt;
  } else if (name == QLatin1String("shift")) {
    value = ModifierKey::Shift;
  } else if (name != QLatin1String("ctrl")) {
    qWarning().noquote() << "tiling: unknown dragFloatModifier" << key << "(ctrl, alt, shift)";
    return;
  }
  if (value == settings_.dragFloatKey) return;
  settings_.dragFloatKey = value;
  pushSettings();
  emit dragFloatModifierChanged();
}

void Tiling::updateDropTarget(const DropTarget& target) {
  QVariantMap next = inactiveDropTarget();
  const bool drawn = target.kind == DropKind::Swap || target.kind == DropKind::Stack ||
                     target.kind == DropKind::Monitor || target.kind == DropKind::Back;
  next[QStringLiteral("kind")] = dropKindText(target.kind);
  if (drawn && !target.rect.isEmpty()) {
    if (QScreen* screen = screenHolding(target.rect.center())) {
      const QRect logical = logicalOn(screen, target.rect);
      next[QStringLiteral("active")] = true;
      next[QStringLiteral("screen")] = screen->name();
      next[QStringLiteral("x")] = logical.x();
      next[QStringLiteral("y")] = logical.y();
      next[QStringLiteral("width")] = logical.width();
      next[QStringLiteral("height")] = logical.height();
    }
  }
  if (next == dropTarget_) return;
  dropTarget_ = std::move(next);
  emit dropTargetChanged();
}

void Tiling::updateResizePreview(const ResizePreview& preview) {
  QVariantMap next = inactiveResizePreview();
  QScreen* screen = preview.rects.empty() ? nullptr : screenHolding(preview.rects.front().center());
  if (screen) {
    QVariantList rects;
    for (const QRect& rect : preview.rects) {
      const QRect logical = logicalOn(screen, rect);
      rects.append(QVariantMap{{QStringLiteral("x"), logical.x()},
                               {QStringLiteral("y"), logical.y()},
                               {QStringLiteral("width"), logical.width()},
                               {QStringLiteral("height"), logical.height()}});
    }
    next[QStringLiteral("active")] = true;
    next[QStringLiteral("screen")] = screen->name();
    next[QStringLiteral("rects")] = rects;
  }
  if (next == resizePreview_) return;
  resizePreview_ = std::move(next);
  emit resizePreviewChanged();
}

// --- commands

void Tiling::toggleFloat() {
  if (active_ && engine_) engine_->setForegroundFloating(std::nullopt);
}

void Tiling::retile() {
  if (active_ && engine_) engine_->retileAll();
}

bool Tiling::setLayout(const QString& name) {
  const bool ok = active_ && engine_ && engine_->setWorkspaceLayout(name);
  refreshCurrentLayout();
  return ok;
}

QString Tiling::cycleLayout(int step) {
  if (!active_ || !engine_) return {};
  const CommandResult result = engine_->cycleLayout(step);
  refreshCurrentLayout();
  return result.message;
}

void Tiling::focusNext() {
  if (active_ && engine_) engine_->focusStep(1);
}

void Tiling::focusPrevious() {
  if (active_ && engine_) engine_->focusStep(-1);
}

QString Tiling::notActive() const {
  if (!engine_) {
    return QStringLiteral(
        "error: the tiler is not available here (offscreen platform or --no-tiling)");
  }
  if (!enabled_) return QStringLiteral("error: tiling is off (set Tiling.enabled in the shell)");
  return QStringLiteral("error: the tiler is not running");
}

QString Tiling::controlFloat(std::optional<bool> floating) {
  if (!active_ || !engine_) return notActive();
  switch (engine_->setForegroundFloating(floating)) {
    case FloatOutcome::NoWindow:
      return QStringLiteral("error: no focused window");
    case FloatOutcome::Floated:
      return QStringLiteral("ok: floating");
    case FloatOutcome::Tiled:
      return QStringLiteral("ok: tiled");
    case FloatOutcome::Unchanged:
      return QStringLiteral("ok: unchanged");
    case FloatOutcome::Ineligible:
      return QStringLiteral("error: this window cannot be tiled");
  }
  return QStringLiteral("error: unexpected");
}

QString Tiling::controlLayout(const QString& name) {
  if (!active_ || !engine_) return notActive();
  if (!engine_->setWorkspaceLayout(name)) {
    return QStringLiteral("error: unknown layout '%1' (%2)")
        .arg(name, layouts().join(QLatin1String(", ")));
  }
  refreshCurrentLayout();
  return QStringLiteral("ok: layout %1").arg(canonicalLayout(name));
}

QString Tiling::controlLayoutStep(int step) {
  const QString answer = run([step](TilingEngine& e) {
    CommandResult result = e.cycleLayout(step);
    result.message = QStringLiteral("layout %1").arg(result.message);
    return result;
  });
  refreshCurrentLayout();
  return answer;
}

QString Tiling::controlRetile() {
  if (!active_ || !engine_) return notActive();
  engine_->retileAll();
  return QStringLiteral("ok: retiling");
}

QString Tiling::controlFocus(int direction) {
  if (!active_ || !engine_) return notActive();
  return engine_->focusStep(direction) ? QStringLiteral("ok: focused")
                                       : QStringLiteral("error: no window to focus");
}

QString Tiling::run(const std::function<CommandResult(TilingEngine&)>& command) {
  if (!active_ || !engine_) return notActive();
  try {
    const CommandResult result = command(*engine_);
    return (result.ok ? QStringLiteral("ok: ") : QStringLiteral("error: ")) + result.message;
  } catch (const std::exception& e) {
    return QStringLiteral("error: %1").arg(QString::fromUtf8(e.what()));
  }
}

QString Tiling::controlFocusDirection(Direction direction) {
  return run([direction](TilingEngine& e) { return e.focusDirection(direction); });
}

QString Tiling::controlMove(Direction direction) {
  return run([direction](TilingEngine& e) { return e.moveDirection(direction); });
}

QString Tiling::controlStack(Direction direction) {
  return run([direction](TilingEngine& e) { return e.stackDirection(direction); });
}

QString Tiling::controlUnstack() {
  return run([](TilingEngine& e) { return e.unstack(); });
}

QString Tiling::controlCycleStack(int step) {
  return run([step](TilingEngine& e) { return e.cycleStack(step); });
}

QString Tiling::controlPromote() {
  return run([](TilingEngine& e) { return e.promote(); });
}

QString Tiling::controlResize(Axis axis, int sign) {
  return run([axis, sign](TilingEngine& e) { return e.resize(axis, sign); });
}

QString Tiling::controlFlip(Axis axis) {
  return run([axis](TilingEngine& e) { return e.flip(axis); });
}

QString Tiling::controlMonocle() {
  return run([](TilingEngine& e) { return e.monocle(); });
}

QString Tiling::controlPause() {
  return run([](TilingEngine& e) { return e.togglePause(); });
}

QString Tiling::controlClose() {
  return run([](TilingEngine& e) { return e.closeForeground(); });
}

QString Tiling::controlMinimize() {
  return run([](TilingEngine& e) { return e.minimizeForeground(); });
}

QString Tiling::controlStatus() {
  const TilingStatus status = engine_ ? engine_->status() : TilingStatus();
  return QStringLiteral(
             "ok: enabled=%1; active=%2; layout=%3; gap=%4; ratio=%5; managed=%6; "
             "floating=%7; workspaces=%8; stacks=%9; clamped=%10; paused=%11; desktop=%12")
      .arg(yesNo(enabled_), yesNo(active_),
           status.layout.isEmpty() ? settings_.layout : status.layout,
           QString::number(settings_.gap), QString::number(settings_.ratio))
      .arg(status.managed)
      .arg(status.floating)
      .arg(status.workspaces)
      .arg(status.stacks)
      .arg(status.clamped)
      .arg(yesNo(status.paused))
      .arg(status.desktop.isEmpty() ? QStringLiteral("unknown") : status.desktop);
}

}  // namespace waku::tiling
