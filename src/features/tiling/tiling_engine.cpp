#include "features/tiling/tiling_engine.h"

#include <QDebug>
#include <QTimer>
#include <algorithm>
#include <cstdlib>
#include <exception>

#include "features/tiling/layouts.h"
#include "features/tiling/tiling_rules.h"

namespace waku::tiling {
namespace {

// After a Cloaked event the engine waits this long to tell "the window was cloaked by a desktop
// switch" from "the window cloaked itself": the registry value that names the shown desktop may
// trail the event.
constexpr int kCloakSettleMs = 150;
// A window counts as moved by the user when any edge of its visible bounds is further than this
// from where the tiler left it (rounding and DPI jitter stay below it).
constexpr int kMovedTolerance = 2;

// "0x1a2b3c zen.exe MozillaWindowClass" for the debug lines: no title, which can be private.
QString describe(WindowId hwnd, const WindowFacts* facts) {
  QString text = QStringLiteral("0x") + QString::number(hwnd, 16);
  if (facts) text += QLatin1Char(' ') + facts->processName + QLatin1Char(' ') + facts->className;
  return text;
}

QString rectText(const QRect& r) {
  return QStringLiteral("%1,%2 %3x%4").arg(r.x()).arg(r.y()).arg(r.width()).arg(r.height());
}

// Ignore verdicts that every menu, tooltip and helper window gets: not worth a line each.
bool quietIgnore(const QString& reason) {
  return reason == QLatin1String("own process") || reason == QLatin1String("tool window") ||
         reason == QLatin1String("ignored class") ||
         reason == QLatin1String("owned by a visible window") ||
         reason == QLatin1String("not visible");
}

bool differs(const QRect& a, const QRect& b) {
  return std::abs(a.x() - b.x()) > kMovedTolerance || std::abs(a.y() - b.y()) > kMovedTolerance ||
         std::abs(a.width() - b.width()) > kMovedTolerance ||
         std::abs(a.height() - b.height()) > kMovedTolerance;
}

// The width or the height differs by more than the tolerance: a resize, not a move.
bool resized(const QRect& a, const QRect& b) {
  return std::abs(a.width() - b.width()) > kMovedTolerance ||
         std::abs(a.height() - b.height()) > kMovedTolerance;
}

QString dropKindName(DropKind kind) {
  switch (kind) {
    case DropKind::None:
      return QStringLiteral("none");
    case DropKind::Swap:
      return QStringLiteral("swap");
    case DropKind::Stack:
      return QStringLiteral("stack");
    case DropKind::Monitor:
      return QStringLiteral("monitor");
    case DropKind::Float:
      return QStringLiteral("float");
    case DropKind::Back:
      return QStringLiteral("back");
  }
  return QString();
}

// --- resizing by mouse

enum class Edge { Left, Right, Top, Bottom };

bool isVertical(Edge edge) { return edge == Edge::Left || edge == Edge::Right; }

// The position of an edge, exclusive on the right and at the bottom.
int edgeAt(const QRect& r, Edge edge) {
  switch (edge) {
    case Edge::Left:
      return r.x();
    case Edge::Right:
      return r.x() + r.width();
    case Edge::Top:
      return r.y();
    case Edge::Bottom:
      return r.y() + r.height();
  }
  return 0;
}

// The slot on the other side of `edge` of slot `index`: a gap away from that edge and overlapping
// it along it (the most, when several do: the main window's edge faces every stacked window).
// Nothing for an outer edge.
std::optional<size_t> slotAcross(const std::vector<QRect>& rects, size_t index, Edge edge,
                                 int gap) {
  const QRect& s = rects[index];
  std::optional<size_t> best;
  int bestOverlap = 0;
  for (size_t i = 0; i < rects.size(); ++i) {
    if (i == index) continue;
    const QRect& t = rects[i];
    int distance = 0;
    switch (edge) {
      case Edge::Left:
        distance = s.x() - (t.x() + t.width());
        break;
      case Edge::Right:
        distance = t.x() - (s.x() + s.width());
        break;
      case Edge::Top:
        distance = s.y() - (t.y() + t.height());
        break;
      case Edge::Bottom:
        distance = t.y() - (s.y() + s.height());
        break;
    }
    const int overlap =
        isVertical(edge) ? std::min(s.y() + s.height(), t.y() + t.height()) - std::max(s.y(), t.y())
                         : std::min(s.x() + s.width(), t.x() + t.width()) - std::max(s.x(), t.x());
    if (std::abs(distance - gap) > kMovedTolerance || overlap <= 0) continue;
    if (!best || overlap > bestOverlap) {
      best = i;
      bestOverlap = overlap;
    }
  }
  return best;
}

// What a drag of an inner edge changes: the workspace's ratio, or the weights of the two windows
// that share the edge.
enum class EdgeRole { None, Ratio, Weights };

// `shape` is the layout as drawn: centerMain with two windows is mainStack.
QString resizeShape(const QString& layout, size_t count) {
  if (layout == QLatin1String("centerMain") && count < 3) return QStringLiteral("mainStack");
  return layout;
}

EdgeRole edgeRole(const QString& shape, size_t index, size_t other, Edge edge) {
  if (shape == QLatin1String("mainStack") || shape == QLatin1String("centerMain")) {
    // The main window's sides are the ratio; the edges between two windows of a column, weights.
    if (index == 0 || other == 0) return isVertical(edge) ? EdgeRole::Ratio : EdgeRole::None;
    return isVertical(edge) ? EdgeRole::None : EdgeRole::Weights;
  }
  if (shape == QLatin1String("columns"))
    return isVertical(edge) ? EdgeRole::Weights : EdgeRole::None;
  if (shape == QLatin1String("rows")) return isVertical(edge) ? EdgeRole::None : EdgeRole::Weights;
  return EdgeRole::None;
}

// The ratio that puts the dragged edge at `at`. The edge is the main window's own (`draggedIsMain`)
// or that of a window across from it, a gap away. mainStack: the main width over the width left
// by the gaps. centerMain: the main window stays centred with equal sides, so its width is twice
// the distance from the centre to the edge.
std::optional<double> ratioFromEdge(const QString& shape, const QRect& area, int gap,
                                    bool draggedIsMain, Edge edge, int at) {
  const bool mainRight = draggedIsMain ? edge == Edge::Right : edge == Edge::Left;
  const int mainEdge = draggedIsMain ? at : (edge == Edge::Left ? at - gap : at + gap);
  const int innerLeft = area.x() + gap;
  const int innerRight = area.x() + area.width() - gap;
  int usable = 0;
  int mainWidth = 0;
  if (shape == QLatin1String("centerMain")) {
    usable = innerRight - innerLeft - 2 * gap;
    mainWidth = mainRight ? 2 * (mainEdge - innerLeft - gap) - usable
                          : usable - 2 * (mainEdge - innerLeft - gap);
  } else {
    usable = innerRight - innerLeft - gap;
    mainWidth = mainRight ? mainEdge - innerLeft : innerRight - mainEdge;
  }
  if (usable <= 0) return std::nullopt;
  return std::clamp(static_cast<double>(mainWidth) / usable, 0.1, 0.9);
}

// The weights of the pair `a` (the dragged window, in `slot`) and `b` (in `otherSlot`) that put the
// shared edge at `at`. The pair keeps its sum and each stays inside the engine's bounds. False
// when nothing changes.
bool shareEdge(WorkspaceView& view, WindowId a, WindowId b, const QRect& slot,
               const QRect& otherSlot, Edge edge, int at) {
  const int pairLength =
      isVertical(edge) ? slot.width() + otherSlot.width() : slot.height() + otherSlot.height();
  if (pairLength <= 1) return false;
  int wanted = 0;
  switch (edge) {
    case Edge::Left:
      wanted = slot.x() + slot.width() - at;
      break;
    case Edge::Right:
      wanted = at - slot.x();
      break;
    case Edge::Top:
      wanted = slot.y() + slot.height() - at;
      break;
    case Edge::Bottom:
      wanted = at - slot.y();
      break;
  }
  const double current = view.weights.count(a) ? view.weights.at(a) : 1.0;
  const double other = view.weights.count(b) ? view.weights.at(b) : 1.0;
  const double pair = current + other;
  const double fraction = static_cast<double>(std::clamp(wanted, 1, pairLength - 1)) / pairLength;
  const double low = std::max(TilingEngine::kMinWeight, pair - TilingEngine::kMaxWeight);
  const double high = std::min(TilingEngine::kMaxWeight, pair - TilingEngine::kMinWeight);
  if (low > high) return false;
  const double next = std::clamp(pair * fraction, low, high);
  const double nextOther = pair - next;
  if (std::abs(next - current) <= 0.0001 && std::abs(nextOther - other) <= 0.0001) return false;
  view.weights[a] = next;
  view.weights[b] = nextOther;
  return true;
}

// Where a window that is floated out of its tile goes: the size it had before the tiler first
// placed it, centred on the tile it leaves, inside the work area. A launch size larger than the
// work area is cut to it.
QRect floatedRect(const QRect& launch, const QRect& tile, const QRect& area) {
  QRect rect(QPoint(), launch.size());
  if (!area.isEmpty()) rect.setSize(rect.size().boundedTo(area.size()));
  rect.moveCenter(tile.center());
  if (area.isEmpty()) return rect;
  rect.moveLeft(std::clamp(rect.left(), area.left(), area.right() - rect.width() + 1));
  rect.moveTop(std::clamp(rect.top(), area.top(), area.bottom() - rect.height() + 1));
  return rect;
}

class QtScheduler final : public Scheduler {
 public:
  void after(int ms, std::function<void()> task) override {
    QTimer::singleShot(ms, [task = std::move(task)] { task(); });
  }
};

}  // namespace

std::unique_ptr<Scheduler> makeQtScheduler() { return std::make_unique<QtScheduler>(); }

TilingEngine::TilingEngine(WindowPlacer& placer, Scheduler& scheduler)
    : placer_(placer), scheduler_(scheduler) {}

TilingEngine::~TilingEngine() = default;

void TilingEngine::defer(int ms, std::function<void()> task) {
  std::weak_ptr<bool> weak = alive_;
  scheduler_.after(ms, [this, weak, task = std::move(task)] {
    if (weak.expired()) return;
    try {
      task();
    } catch (const std::exception& e) {
      qWarning().noquote() << "tiling: task failed:" << e.what();
    } catch (...) {
      qWarning() << "tiling: task failed";
    }
  });
}

void TilingEngine::setSettings(const TilingSettings& settings) {
  const bool layoutChanged = settings.layout != settings_.layout;
  const bool ratioChanged = settings.ratio != settings_.ratio;
  if (running_ && layoutChanged) {
    // Every workspace takes the new default: those that change layout start from its sizes.
    const QString next = canonicalLayout(settings.layout);
    for (const WorkspaceKey& key : model_.keys()) {
      if (canonicalLayout(model_.layout(key, settings_.layout)) != next) model_.resetSizes(key);
    }
  }
  settings_ = settings;
  if (!running_) return;
  if (layoutChanged) model_.clearLayouts();
  if (ratioChanged) model_.clearRatios();
  retileAll();
}

void TilingEngine::start() {
  if (running_) return;
  running_ = true;
  currentDesktop_ = placer_.currentDesktop();
  // Let the shell finish setting its rules before taking over existing windows.
  for (const WindowId hwnd : placer_.candidates()) scheduleEvaluate(hwnd, 0, 0, false, true);
  retileAll();
}

void TilingEngine::stop() {
  running_ = false;
  paused_ = false;
  model_.clear();
  ignored_.clear();
  pending_.clear();
  restoreBeforeTile_.clear();
  moving_.clear();
  placed_.clear();
  floatRects_.clear();
  resizeDrags_.clear();
  moveDrags_.clear();
  dropped_.clear();
  publishDrop({});
  clamped_.clear();
  settling_.clear();
  verifying_.clear();
  wasTop_.clear();
  retiles_.clear();
  flushScheduled_ = false;
  currentDesktop_.clear();
  alive_ = std::make_shared<bool>(true);  // the old token expires with its deferred tasks
}

// --- events

void TilingEngine::handle(const WindowEvent& event) {
  if (!running_ || event.hwnd == 0) return;
  const WindowId hwnd = event.hwnd;
  switch (event.kind) {
    case EventKind::Show:
    case EventKind::MinimizeEnd: {
      // Tooltips, menus and other transient windows show all the time: skip those that were
      // rejected for good.
      if (model_.contains(hwnd)) break;
      const auto rejected = ignored_.find(hwnd);
      if (rejected != ignored_.end() && rejected->second == Ignored::Lasting) break;
      scheduleEvaluate(hwnd, 0, 0, false);
      break;
    }
    case EventKind::NameChange: {
      // Frequent (terminals and browsers rename their windows all the time): look again only at a
      // window that is new to the tiler or was rejected for want of a title.
      if (model_.contains(hwnd) || model_.isFloating(hwnd)) break;
      const auto rejected = ignored_.find(hwnd);
      if (rejected != ignored_.end() && rejected->second != Ignored::Untitled) break;
      scheduleEvaluate(hwnd, 0, 0, false);
      break;
    }
    case EventKind::Uncloaked:
      refreshDesktop();
      if (const auto key = model_.keyOf(hwnd)) {
        requestRetile(*key);
      } else {
        scheduleEvaluate(hwnd, 0, 0, false);
      }
      break;
    case EventKind::Cloaked:
      if (model_.contains(hwnd)) defer(kCloakSettleMs, [this, hwnd] { onCloaked(hwnd); });
      break;
    case EventKind::Hide: {
      if (!model_.contains(hwnd)) break;  // most hides are of windows the tiler never held
      const std::optional<WindowFacts> facts = placer_.state(hwnd);
      if (facts && facts->visible) break;  // a hide of a child or a transient state
      forget(hwnd, false);
      break;
    }
    case EventKind::Destroy:
      forget(hwnd, true);
      ignored_.erase(hwnd);
      pending_.erase(hwnd);
      break;
    case EventKind::MinimizeStart:
      forget(hwnd, false);
      break;
    case EventKind::MoveSizeStart:
      onMoveSizeStart(hwnd);
      break;
    case EventKind::MoveSizeEnd:
      onMoveSizeEnd(hwnd);
      break;
    case EventKind::LocationChange:
      onLocationChange(hwnd);
      onSettlingMove(hwnd);
      break;
    case EventKind::Foreground:
      refreshDesktop();
      // A window that is under the top of its stack (alt-tab, the taskbar) takes the top.
      if (model_.raiseInStack(hwnd)) {
        if (const auto key = model_.keyOf(hwnd)) requestRetile(*key);
      }
      break;
  }
}

void TilingEngine::onCloaked(WindowId hwnd) {
  if (!running_ || !model_.contains(hwnd)) return;
  refreshDesktop();
  const std::optional<WindowFacts> facts = placer_.query(hwnd);
  if (!facts) {
    forget(hwnd, true);
    return;
  }
  if (!facts->cloaked) return;
  // Cloaked because its desktop is not the shown one. A desktop switch cloaks every window of the
  // desktop it leaves: those keep their place in that desktop's stack. A window that was filed
  // under the shown desktop and now belongs to another was moved away (Task View, `wakuc desktop
  // move`): it leaves this stack and the others fill the gap.
  if (!facts->desktopId.isEmpty() && !currentDesktop_.isEmpty() &&
      facts->desktopId != currentDesktop_) {
    const std::optional<WorkspaceKey> key = model_.keyOf(hwnd);
    if (key && key->desktop == currentDesktop_) forget(hwnd, false);
    return;
  }
  forget(hwnd, false);
}

void TilingEngine::onMoveSizeStart(WindowId hwnd) {
  if (!model_.contains(hwnd)) return;
  moving_.insert(hwnd);
  const std::optional<WindowFacts> facts = placer_.state(hwnd);
  if (!facts) return;
  // The bounds now are what the end is compared with: a clamped window counts as unmoved.
  placed_[hwnd] = facts->rect;
  const std::optional<WorkspaceKey> key = model_.keyOf(hwnd);
  if (!key || paused_) return;
  const std::vector<WindowId> active = activeWindows(*key);
  const auto at = std::find(active.begin(), active.end(), hwnd);
  if (at == active.end()) return;
  const QRect area = placer_.workArea(key->monitor);
  if (area.isEmpty()) return;
  const std::vector<QRect> slotRects = slotsFor(*key, area, active);
  // Whether this is a move or a resize shows only once the window changes: both start here.
  if (settings_.dragAction == DragAction::Swap && model_.view(*key).monocle == 0) {
    MoveDrag moveDrag;
    moveDrag.key = *key;
    moveDrag.startRect = facts->rect;
    moveDrag.active = active;
    moveDrag.slotRects = slotRects;
    moveDrags_[hwnd] = std::move(moveDrag);
  }
  const QString layout = canonicalLayout(model_.layout(*key, settings_.layout));
  if (!resizesByMouse(layout)) return;
  ResizeDrag drag;
  drag.key = *key;
  drag.layout = layout;
  drag.startRect = facts->rect;
  drag.area = area;
  drag.active = active;
  drag.slotRects = slotRects;
  drag.initialRatio = model_.view(*key).ratio;
  drag.initialWeights = model_.view(*key).weights;
  drag.gap = std::clamp(
      settings_.gap, 0,
      std::min(area.width(), area.height()) / (2 * (static_cast<int>(active.size()) + 1)));
  resizeDrags_[hwnd] = std::move(drag);
}

void TilingEngine::onMoveSizeEnd(WindowId hwnd) {
  if (!moving_.count(hwnd)) return;
  // The final bounds can be newer than the last location event, or the only sample.
  onLocationChange(hwnd);
  moving_.erase(hwnd);
  std::optional<MoveDrag> moveDrag;
  if (const auto found = moveDrags_.find(hwnd); found != moveDrags_.end()) {
    moveDrag = std::move(found->second);
    endMoveDrag(hwnd);
  }
  const auto drag = resizeDrags_.find(hwnd);
  const bool hasResizeDrag = drag != resizeDrags_.end();
  const bool wasResize = hasResizeDrag && drag->second.resized;
  const bool snapped = hasResizeDrag && drag->second.snapped;
  const QRect startRect = hasResizeDrag ? drag->second.startRect : QRect();
  const std::optional<double> initialRatio =
      hasResizeDrag ? drag->second.initialRatio : std::nullopt;
  const std::map<WindowId, double> initialWeights =
      hasResizeDrag ? drag->second.initialWeights : std::map<WindowId, double>{};
  resizeDrags_.erase(hwnd);
  if (!model_.contains(hwnd)) return;
  if (paused_) return;  // nothing was placed, so there is nothing to compare with
  const std::optional<WindowFacts> facts = placer_.state(hwnd);
  const std::optional<WorkspaceKey> key = model_.keyOf(hwnd);
  if (!facts) {
    forget(hwnd, true);
    return;
  }
  const bool sizeChanged =
      hasResizeDrag && (std::abs(startRect.width() - facts->rect.width()) > kMovedTolerance ||
                        std::abs(startRect.height() - facts->rect.height()) > kMovedTolerance);
  // A Swap drag that moved the window lands where the cursor is. A change of size is a resize or
  // a Snap (Windows sized it at the release): those keep the behaviour below.
  if (moveDrag && key && differs(moveDrag->startRect, facts->rect) &&
      !resized(moveDrag->startRect, facts->rect) && dropMoved(hwnd, *key, *moveDrag)) {
    return;
  }
  const bool cancelled = hasResizeDrag && wasResize && !differs(startRect, facts->rect);
  if ((cancelled || snapped) && key) {
    WorkspaceView& view = model_.edit(*key);
    view.ratio = initialRatio;
    view.weights = initialWeights;
  }
  const auto placed = placed_.find(hwnd);
  // A Snap is the user placing the window by hand, as a move is.
  if (snapped || (!cancelled && !wasResize && !sizeChanged && placed != placed_.end() &&
                  differs(placed->second, facts->rect))) {
    // The user took this window out of the layout.
    model_.setFloating(hwnd, true);
    placed_.erase(hwnd);
  }
  if (key) requestRetile(*key);  // the others fill the gap, or the window snaps back
}

void TilingEngine::onLocationChange(WindowId hwnd) {
  updateMoveDrag(hwnd);
  auto found = resizeDrags_.find(hwnd);
  if (!running_ || found == resizeDrags_.end() || !model_.contains(hwnd)) return;
  const std::optional<WindowFacts> facts = placer_.state(hwnd);
  if (!facts) {
    forget(hwnd, true);
    return;
  }
  ResizeDrag& drag = found->second;
  const QRect& rect = facts->rect;
  if (std::abs(drag.startRect.width() - rect.width()) > kMovedTolerance ||
      std::abs(drag.startRect.height() - rect.height()) > kMovedTolerance) {
    drag.resized = true;
  }
  // A border drag moves one edge per axis (two at a corner). Both edges of an axis at once is
  // Windows placing the window, a Snap at the release: that is not a resize of the tiles.
  const auto edgeMoved = [&](Edge edge) {
    return std::abs(edgeAt(rect, edge) - edgeAt(drag.startRect, edge)) > kMovedTolerance;
  };
  if (drag.resized && ((edgeMoved(Edge::Left) && edgeMoved(Edge::Right)) ||
                       (edgeMoved(Edge::Top) && edgeMoved(Edge::Bottom)))) {
    drag.snapped = true;
  }
  if (paused_ || !drag.resized || drag.snapped || drag.slotRects.size() != drag.active.size() ||
      drag.active.size() < 2) {
    return;
  }

  const auto at = std::find(drag.active.begin(), drag.active.end(), hwnd);
  if (at == drag.active.end()) return;
  const size_t index = static_cast<size_t>(at - drag.active.begin());
  const QString shape = resizeShape(drag.layout, drag.active.size());
  WorkspaceView& view = model_.edit(drag.key);
  bool changed = false;
  // Each edge that moved: an inner one moves the boundary it shares with the tile across it, an
  // outer one (on the work area) goes back at the release.
  for (const Edge edge : {Edge::Left, Edge::Right, Edge::Top, Edge::Bottom}) {
    if (!edgeMoved(edge)) continue;
    const std::optional<size_t> other = slotAcross(drag.slotRects, index, edge, drag.gap);
    if (!other) continue;
    const int position = edgeAt(rect, edge);
    switch (edgeRole(shape, index, *other, edge)) {
      case EdgeRole::Ratio: {
        const std::optional<double> ratio =
            ratioFromEdge(shape, drag.area, drag.gap, index == 0, edge, position);
        if (ratio && (!view.ratio || std::abs(*view.ratio - *ratio) > 0.0001)) {
          view.ratio = *ratio;
          changed = true;
        }
        break;
      }
      case EdgeRole::Weights:
        changed |= shareEdge(view, hwnd, drag.active[*other], drag.slotRects[index],
                             drag.slotRects[*other], edge, position);
        break;
      case EdgeRole::None:
        break;
    }
  }
  if (changed) requestRetile(drag.key);
}

// --- drag to swap

void TilingEngine::updateMoveDrag(WindowId hwnd) {
  const auto found = moveDrags_.find(hwnd);
  if (found == moveDrags_.end()) return;
  if (!running_ || paused_ || !model_.contains(hwnd)) {
    endMoveDrag(hwnd);
    return;
  }
  const std::optional<WindowFacts> facts = placer_.state(hwnd);
  if (!facts) return;
  MoveDrag& drag = found->second;
  if (resized(drag.startRect, facts->rect)) {
    endMoveDrag(hwnd);  // a resize: the resize drag (or a float at the end) takes it
    return;
  }
  if (!drag.moved) {
    if (!differs(drag.startRect, facts->rect)) return;  // a click on the title bar shows nothing
    drag.moved = true;
  }
  publishDrop(dropTargetFor(hwnd, drag));
}

DropTarget TilingEngine::dropTargetFor(WindowId hwnd, MoveDrag& drag) {
  DropTarget target;
  target.monitor = drag.key.monitor;
  if (placer_.heldKeys().holds(settings_.dragFloatKey)) {
    target.kind = DropKind::Float;
    return target;
  }
  // By default it goes back to its own tile.
  target.kind = DropKind::Back;
  const auto self = std::find(drag.active.begin(), drag.active.end(), hwnd);
  const size_t own = static_cast<size_t>(self - drag.active.begin());
  if (own < drag.slotRects.size()) target.rect = drag.slotRects[own];
  const std::optional<QPoint> cursor = placer_.cursorPosition();
  if (!cursor) return target;
  const MonitorId monitor = placer_.monitorAt(*cursor);
  if (monitor != 0 && monitor != drag.key.monitor) {
    auto spot = drag.spots.find(monitor);
    if (spot == drag.spots.end()) {
      spot = drag.spots.emplace(monitor, spotAtEnd(WorkspaceKey{monitor, drag.key.desktop})).first;
    }
    if (!spot->second.isEmpty()) {
      target.kind = DropKind::Monitor;
      target.monitor = monitor;
      target.rect = spot->second;
    }
    return target;
  }
  for (size_t i = 0; i < drag.active.size() && i < drag.slotRects.size(); ++i) {
    if (!drag.slotRects[i].contains(*cursor)) continue;
    const WindowId other = drag.active[i];
    if (other == hwnd) return target;
    target.kind = model_.stackOf(other).size() > 1 ? DropKind::Stack : DropKind::Swap;
    target.window = other;
    target.rect = drag.slotRects[i];
    return target;
  }
  return target;  // over a gap between tiles
}

QRect TilingEngine::spotAtEnd(const WorkspaceKey& key) {
  const QRect area = placer_.workArea(key.monitor);
  if (area.isEmpty()) return QRect();
  std::vector<WindowId> active = activeWindows(key);
  active.push_back(0);  // the dropped window, appended like a new one
  const std::vector<QRect> rects = slotsFor(key, area, active);
  return rects.empty() ? QRect() : rects.back();
}

bool TilingEngine::dropMoved(WindowId hwnd, const WorkspaceKey& key, MoveDrag& drag) {
  const DropTarget target = dropTargetFor(hwnd, drag);
  if (target.kind == DropKind::Float) return false;  // floats like a drag under DragAction::Float
  bool applied = false;
  switch (target.kind) {
    case DropKind::Swap:
      applied = model_.swap(hwnd, target.window);
      break;
    case DropKind::Stack:
      applied = model_.stackOnto(hwnd, target.window);
      break;
    case DropKind::Monitor: {
      const WorkspaceKey to{target.monitor, key.desktop};
      model_.add(to, hwnd);
      placed_.erase(hwnd);
      dropped_.insert(hwnd);
      requestRetile(to);
      applied = true;
      break;
    }
    case DropKind::None:
    case DropKind::Float:
    case DropKind::Back:
      break;
  }
  qDebug().noquote() << "tiling: drop" << describe(hwnd, nullptr)
                     << (applied ? dropKindName(target.kind) : QStringLiteral("back"));
  requestRetile(key);  // the tiles take their new places, or the window goes back to its own
  return true;
}

void TilingEngine::endMoveDrag(WindowId hwnd) {
  if (moveDrags_.erase(hwnd) == 0) return;
  publishDrop({});
}

void TilingEngine::publishDrop(const DropTarget& target) {
  if (target == drop_) return;
  drop_ = target;
  if (dropListener_) dropListener_(drop_);
}

// --- evaluation

void TilingEngine::scheduleEvaluate(WindowId hwnd, int ms, int attempt, bool force,
                                    bool initialScan) {
  if (!pending_.insert(hwnd).second) return;
  defer(ms,
        [this, hwnd, attempt, force, initialScan] { evaluate(hwnd, attempt, force, initialScan); });
}

void TilingEngine::evaluate(WindowId hwnd, int attempt, bool force, bool initialScan) {
  if (!running_) return;
  pending_.erase(hwnd);
  const std::optional<WindowFacts> facts = placer_.query(hwnd);
  if (!facts) {
    forget(hwnd, true);
    ignored_.erase(hwnd);
    return;
  }
  if (model_.contains(hwnd)) return;
  if (model_.isFloating(hwnd) && !force) return;

  if ((initialScan || force) && facts->zoomed && !facts->iconic) restoreBeforeTile_.insert(hwnd);
  if (facts->iconic) restoreBeforeTile_.erase(hwnd);
  WindowFacts candidate = *facts;
  if (restoreBeforeTile_.count(hwnd) && facts->zoomed) {
    candidate.zoomed = false;
    // A maximized window can cover the monitor before the app bar reserves its work area.
    candidate.fullscreen = false;
  }
  const Eligibility eligibility =
      classify(candidate, settings_.rules, settings_.selfElevated, force);
  if (eligibility.verdict == Verdict::Ignore && !quietIgnore(eligibility.reason)) {
    qDebug().noquote() << "tiling: ignore" << describe(hwnd, &*facts) << "-" << eligibility.reason;
  } else if (eligibility.verdict == Verdict::Float) {
    qDebug().noquote() << "tiling: float" << describe(hwnd, &*facts) << "-" << eligibility.reason;
  } else if (eligibility.verdict == Verdict::Retry && attempt >= kMaxRetries) {
    qDebug().noquote() << "tiling: gave up on" << describe(hwnd, &*facts) << "-"
                       << eligibility.reason;
  } else if (eligibility.verdict == Verdict::Manage) {
    qDebug().noquote() << "tiling: manage" << describe(hwnd, &*facts) << "at"
                       << rectText(facts->rect) << (eligibility.settle ? "(settle rule)" : "");
  }
  switch (eligibility.verdict) {
    case Verdict::Ignore:
      if (eligibility.reason != QStringLiteral("cloaked")) restoreBeforeTile_.erase(hwnd);
      ignored_[hwnd] = eligibility.lasting ? Ignored::Lasting : Ignored::Transient;
      break;
    case Verdict::Float:
      restoreBeforeTile_.erase(hwnd);
      ignored_.erase(hwnd);
      model_.setFloating(hwnd, true);
      break;
    case Verdict::Retry:
      if (attempt < kMaxRetries) {
        scheduleEvaluate(hwnd, kRetryMs, attempt + 1, force);
      } else {
        ignored_[hwnd] = eligibility.reason == QLatin1String("untitled") ? Ignored::Untitled
                                                                         : Ignored::Transient;
      }
      break;
    case Verdict::Manage: {
      ignored_.erase(hwnd);
      const WorkspaceKey key = keyFor(*facts);
      model_.add(key, hwnd);
      if (eligibility.settle) startSettling(hwnd);
      requestRetile(key);
      break;
    }
  }
}

void TilingEngine::forget(WindowId hwnd, bool dropFloat) {
  restoreBeforeTile_.erase(hwnd);
  placed_.erase(hwnd);
  // dropFloat: the window is gone, not just out of the layout for now.
  if (dropFloat) floatRects_.erase(hwnd);
  clamped_.erase(hwnd);
  settling_.erase(hwnd);
  verifying_.erase(hwnd);
  moving_.erase(hwnd);
  resizeDrags_.erase(hwnd);
  endMoveDrag(hwnd);
  dropped_.erase(hwnd);
  wasTop_.erase(hwnd);
  const std::optional<WorkspaceKey> key = model_.remove(hwnd);
  if (dropFloat) model_.setFloating(hwnd, false);
  if (key) requestRetile(*key);
}

WorkspaceKey TilingEngine::keyFor(const WindowFacts& facts) const {
  return WorkspaceKey{facts.monitor, facts.desktopId.isEmpty() ? currentDesktop_ : facts.desktopId};
}

void TilingEngine::refreshDesktop() {
  const QString desktop = placer_.currentDesktop();
  if (desktop.isEmpty() || desktop == currentDesktop_) return;
  currentDesktop_ = desktop;
  for (const WorkspaceKey& key : model_.keys()) {
    if (key.desktop == desktop) requestRetile(key);
  }
}

// --- placement

void TilingEngine::retileAll() {
  if (!running_) return;
  for (const WorkspaceKey& key : model_.keys()) {
    if (currentDesktop_.isEmpty() || key.desktop == currentDesktop_) requestRetile(key);
  }
}

void TilingEngine::requestRetile(const WorkspaceKey& key) {
  retiles_.insert(key);
  if (flushScheduled_) return;
  flushScheduled_ = true;
  defer(0, [this] { flushRetiles(); });
}

void TilingEngine::flushRetiles() {
  flushScheduled_ = false;
  if (!running_) return;
  const std::set<WorkspaceKey> keys = std::move(retiles_);
  retiles_.clear();
  for (const WorkspaceKey& key : keys) retile(key);
}

void TilingEngine::retile(const WorkspaceKey& key) {
  if (!running_ || paused_) return;
  // Never place a window of a desktop that is not shown: it is cloaked.
  if (!currentDesktop_.isEmpty() && key.desktop != currentDesktop_) return;
  const std::vector<Slot> order =
      model_.slotList(key);  // a copy: forgetting a window edits the model
  if (order.empty()) return;
  const QRect area = placer_.workArea(key.monitor);
  if (area.isEmpty()) return;

  // Every slot gives one window (its top) to the layout. The windows under the top follow it to
  // the same rectangle.
  std::vector<WindowId> active;
  std::map<WindowId, QRect> current;
  std::map<WindowId, std::vector<WindowId>> beneath;  // top -> the windows under it
  for (const Slot& slot : order) {
    std::vector<WindowId> hidden;
    bool topActive = false;
    for (const WindowId hwnd : slot.windows) {
      const std::optional<WindowFacts> facts = placer_.state(hwnd);
      if (!facts) {
        forget(hwnd, true);
        continue;
      }
      if (!facts->visible) {
        forget(hwnd, false);
        continue;
      }
      // A window dropped on this monitor is put here, wherever most of it is until then.
      const bool dropped = dropped_.erase(hwnd) != 0;
      if (!dropped && facts->monitor != 0 && facts->monitor != key.monitor) {
        // Moved to another monitor by something other than a drag (a hotkey, a script).
        const WorkspaceKey moved{facts->monitor, key.desktop};
        model_.add(moved, hwnd);
        placed_.erase(hwnd);
        requestRetile(moved);
        requestRetile(key);
        continue;
      }
      if (facts->iconic || facts->cloaked) continue;
      if (facts->zoomed) {
        if (restoreBeforeTile_.erase(hwnd) != 0) {
          if (placer_.restore(hwnd)) {
            defer(kRetryMs, [this, hwnd] { waitForRestore(hwnd, 0); });
          } else {
            forget(hwnd, false);
            ignored_[hwnd] = Ignored::Lasting;
          }
        }
        continue;
      }
      restoreBeforeTile_.erase(hwnd);
      if (facts->fullscreen) continue;
      current[hwnd] = facts->rect;
      // The first time the tiler is about to place it: remember the size the window chose. A later
      // float-to-tile updates it (setForegroundFloating).
      floatRects_.emplace(hwnd, facts->rect);
      if (hwnd == slot.topWindow()) {
        active.push_back(hwnd);
        topActive = true;
      } else {
        hidden.push_back(hwnd);
      }
    }
    if (topActive) beneath[slot.topWindow()] = std::move(hidden);
  }

  std::vector<QRect> rects = slotsFor(key, area, active);
  const WindowId monocle = model_.view(key).monocle;
  if (monocle != 0 && std::find(active.begin(), active.end(), monocle) != active.end()) {
    // The monocle window fills the area; the others stay where they are.
    rects = {layoutSlots(settings_.layout, area, 1, settings_.gap, settings_.ratio).front()};
    active = {monocle};
  }
  std::vector<std::pair<WindowId, QRect>> placedNow;
  auto placeOne = [&](WindowId hwnd, const QRect& slot) {
    if (moving_.count(hwnd)) return;  // the user holds it: its slot stays empty
    if (current[hwnd] == slot) {
      placed_[hwnd] = slot;
      clamped_.erase(hwnd);
      return;
    }
    // Before the call: the location event of this placement can arrive while it runs.
    verifying_.insert(hwnd);
    qDebug().noquote() << "tiling: place" << describe(hwnd, nullptr) << "from"
                       << rectText(current[hwnd]) << "to" << rectText(slot);
    const PlaceResult result = placer_.place(hwnd, slot);
    if (!result.ok) {
      // It refused (elevated, gone): leave it alone from now on.
      forget(hwnd, false);
      ignored_[hwnd] = Ignored::Lasting;
      return;
    }
    // A window that clamped keeps its slot and leaves a gap; its achieved bounds are what the
    // next MoveSizeEnd is compared with.
    placed_[hwnd] = result.achieved;
    placedNow.emplace_back(hwnd, slot);
  };
  for (size_t i = 0; i < active.size() && i < rects.size(); ++i) {
    placeOne(active[i], rects[i]);
    for (const WindowId hidden : beneath[active[i]]) placeOne(hidden, rects[i]);
  }
  raiseNewTops(key, active);
  if (!placedNow.empty()) {
    defer(kVerifyMs, [this, placedNow] { verifyPlacements(placedNow); });
  }
}

void TilingEngine::waitForRestore(WindowId hwnd, int attempt) {
  if (!running_ || !model_.contains(hwnd)) return;
  const std::optional<WindowFacts> facts = placer_.state(hwnd);
  if (!facts) {
    forget(hwnd, true);
    return;
  }
  if (facts->zoomed) {
    if (attempt < kMaxRetries) {
      defer(kRetryMs, [this, hwnd, attempt] { waitForRestore(hwnd, attempt + 1); });
    }
    return;
  }
  if (const auto key = model_.keyOf(hwnd)) requestRetile(*key);
}

// A stack shows its top: when the top of a stack is not the one the last retile left on top, it
// is brought to the front of the windows it shares a rectangle with.
void TilingEngine::raiseNewTops(const WorkspaceKey& key, const std::vector<WindowId>& active) {
  std::set<WindowId> before;
  std::set<WindowId> tops;
  for (const Slot& slot : model_.slotList(key)) {
    for (const WindowId hwnd : slot.windows) {
      if (wasTop_.erase(hwnd) != 0) before.insert(hwnd);
    }
    if (slot.windows.size() > 1) tops.insert(slot.topWindow());
  }
  for (const WindowId top : tops) {
    wasTop_.insert(top);
    if (before.count(top) == 0 && std::find(active.begin(), active.end(), top) != active.end()) {
      placer_.raise(top);
    }
  }
}

void TilingEngine::verifyPlacements(const std::vector<std::pair<WindowId, QRect>>& placed) {
  for (const auto& [hwnd, slot] : placed) {
    verifying_.erase(hwnd);
    if (!model_.contains(hwnd) || moving_.count(hwnd)) continue;
    const std::optional<WindowFacts> facts = placer_.state(hwnd);
    if (!facts) continue;
    placed_[hwnd] = facts->rect;
    if (!differs(facts->rect, slot)) {
      clamped_.erase(hwnd);
      continue;
    }
    const QString found = QStringLiteral("readback found %1 instead of %2")
                              .arg(rectText(facts->rect), rectText(slot));
    // A window the engine has just started managing may be putting back the bounds it saved.
    if (settleLater(hwnd, *facts, found)) continue;
    // Otherwise (or once its placements are spent) a window that cannot take its slot keeps what
    // it took.
    qDebug().noquote() << "tiling:" << describe(hwnd, &*facts) << found << "- clamped, left there";
    clamped_.insert(hwnd);
  }
}

void TilingEngine::startSettling(WindowId hwnd) {
  const unsigned period = ++settleSerial_;
  settling_[hwnd] = Settling{kSettlePlacements, period, 0};
  defer(kSettleMs, [this, hwnd, period] {
    const auto found = settling_.find(hwnd);
    if (found != settling_.end() && found->second.period == period) settling_.erase(found);
  });
}

bool TilingEngine::settleLater(WindowId hwnd, const WindowFacts& facts, const QString& why) {
  const auto settling = settling_.find(hwnd);
  if (paused_ || settling == settling_.end() || settling->second.left <= 0) return false;
  if (facts.iconic || facts.zoomed || facts.fullscreen || facts.cloaked) return false;
  // The window may not be done: wait until it has been quiet, so the placement comes after its
  // last step instead of between two of them.
  const unsigned quiet = ++settleSerial_;
  settling->second.quiet = quiet;
  qDebug().noquote() << "tiling:" << describe(hwnd, &facts) << why
                     << "- placing again once it is quiet";
  defer(kSettleQuietMs, [this, hwnd, quiet] {
    const auto found = settling_.find(hwnd);
    if (!running_ || found == settling_.end() || found->second.quiet != quiet ||
        found->second.left <= 0) {
      return;
    }
    --found->second.left;
    clamped_.erase(hwnd);
    if (const auto key = model_.keyOf(hwnd)) requestRetile(*key);
  });
  return true;
}

void TilingEngine::onSettlingMove(WindowId hwnd) {
  if (!running_ || paused_ || !model_.contains(hwnd)) return;
  // Until the readback the bounds can be those of our own posted placement; it decides then.
  if (verifying_.count(hwnd) || moving_.count(hwnd)) return;
  const auto placed = placed_.find(hwnd);
  if (placed == placed_.end()) return;  // not placed yet: the first placement takes care of it
  const std::optional<WindowFacts> facts = placer_.state(hwnd);
  if (!facts || facts->iconic || facts->zoomed || facts->fullscreen || facts->cloaked) return;
  if (!differs(placed->second, facts->rect)) return;
  // Noted once per change: the next event of the same bounds finds nothing new.
  placed->second = facts->rect;
  const QString moved = QStringLiteral("moved itself to %1").arg(rectText(facts->rect));
  if (settleLater(hwnd, *facts, moved)) return;
  // Its own change, after the settling period or once its placements are spent: it stays.
  qDebug().noquote() << "tiling:" << describe(hwnd, &*facts) << moved << "- left there";
}

bool TilingEngine::resizesByMouse(const QString& layout) {
  return layout == QLatin1String("mainStack") || layout == QLatin1String("columns") ||
         layout == QLatin1String("rows") || layout == QLatin1String("centerMain");
}

std::vector<QRect> TilingEngine::slotsFor(const WorkspaceKey& key, const QRect& area,
                                          const std::vector<WindowId>& active) const {
  const WorkspaceView& view = model_.view(key);
  std::vector<double> weights;
  weights.reserve(active.size());
  for (const WindowId hwnd : active) {
    const auto found = view.weights.find(hwnd);
    weights.push_back(found == view.weights.end() ? 1.0 : found->second);
  }
  std::vector<QRect> rects =
      layoutSlots(model_.layout(key, settings_.layout), area, static_cast<int>(active.size()),
                  settings_.gap, view.ratio.value_or(settings_.ratio), weights);
  if (view.flipHorizontal || view.flipVertical) {
    rects = flipSlots(std::move(rects), area, view.flipHorizontal, view.flipVertical);
  }
  return rects;
}

// --- commands

FloatOutcome TilingEngine::setForegroundFloating(std::optional<bool> floating) {
  if (!running_) return FloatOutcome::NoWindow;
  const std::optional<WindowId> front = placer_.foreground();
  if (!front) return FloatOutcome::NoWindow;
  const WindowId hwnd = *front;
  const bool managed = model_.contains(hwnd);
  const std::optional<WindowFacts> facts = placer_.state(hwnd);
  const bool maximized = facts && facts->zoomed && !facts->iconic;
  const bool wantFloat = floating.value_or(managed && !maximized);
  if (wantFloat) {
    if (!managed) return FloatOutcome::Unchanged;
    const std::optional<WorkspaceKey> key = model_.keyOf(hwnd);
    model_.setFloating(hwnd, true);
    placed_.erase(hwnd);
    // Give the window back the size it had before it was tiled, centred where its tile was, so a
    // float does not leave it at the size of a slot it no longer has.
    const auto launch = floatRects_.find(hwnd);
    if (launch != floatRects_.end() && facts && !facts->iconic && !facts->zoomed &&
        !facts->fullscreen) {
      const QRect area = placer_.workArea(facts->monitor);
      placer_.place(hwnd, floatedRect(launch->second, facts->rect, area));
    }
    if (key) requestRetile(*key);
    return FloatOutcome::Floated;
  }
  if (managed) {
    if (!maximized) return FloatOutcome::Unchanged;
    restoreBeforeTile_.insert(hwnd);
    if (const auto key = model_.keyOf(hwnd)) requestRetile(*key);
    return FloatOutcome::Tiled;
  }
  // A floating window goes back into a tile: the size the user gave it while it floated is the one
  // the next float gives back.
  if (facts && !facts->iconic && !facts->zoomed && !facts->fullscreen && !facts->rect.isEmpty()) {
    floatRects_[hwnd] = facts->rect;
  }
  pending_.erase(hwnd);
  evaluate(hwnd, 0, true);
  return model_.contains(hwnd) ? FloatOutcome::Tiled : FloatOutcome::Ineligible;
}

std::optional<WorkspaceKey> TilingEngine::foregroundKey() const {
  if (const std::optional<WindowId> front = placer_.foreground()) {
    if (const auto key = model_.keyOf(*front)) return key;
  }
  return std::nullopt;
}

bool TilingEngine::setWorkspaceLayout(const QString& name) {
  const QString layout = canonicalLayout(name);
  if (layout.isEmpty()) return false;
  // Sizes given by hand belong to the layout they were given in: another starts from its defaults.
  if (const std::optional<WorkspaceKey> key = foregroundKey()) {
    if (canonicalLayout(model_.layout(*key, settings_.layout)) != layout) model_.resetSizes(*key);
    model_.setLayout(*key, layout);
    requestRetile(*key);
  } else {
    for (const WorkspaceKey& other : model_.keys()) {
      const bool ownLayout = !model_.layout(other, QString()).isEmpty();
      if (!ownLayout && canonicalLayout(settings_.layout) != layout) model_.resetSizes(other);
    }
    settings_.layout = layout;
    retileAll();
  }
  return true;
}

QString TilingEngine::currentLayout() const {
  QString name = settings_.layout;
  if (const std::optional<WorkspaceKey> key = foregroundKey()) name = model_.layout(*key, name);
  const QString canonical = canonicalLayout(name);
  return canonical.isEmpty() ? layoutNames().front() : canonical;
}

CommandResult TilingEngine::cycleLayout(int step) {
  const QStringList names = layoutNames();
  const qsizetype size = names.size();
  const qsizetype at = std::max<qsizetype>(0, names.indexOf(currentLayout()));
  const QString next = names.at(static_cast<qsizetype>(((at + step) % size + size) % size));
  setWorkspaceLayout(next);
  return {true, next};
}

bool TilingEngine::focusStep(int direction) {
  if (!running_ || direction == 0) return false;
  const std::optional<WindowId> front = placer_.foreground();
  std::optional<WorkspaceKey> key = front ? model_.keyOf(*front) : std::nullopt;
  if (!key) {
    for (const WorkspaceKey& candidate : model_.keys()) {
      if (candidate.desktop == currentDesktop_ || currentDesktop_.isEmpty()) {
        key = candidate;
        break;
      }
    }
  }
  if (!key) return false;
  const std::vector<WindowId>& stack = model_.stack(*key);
  if (stack.empty()) return false;
  const auto at = front ? std::find(stack.begin(), stack.end(), *front) : stack.end();
  const int count = static_cast<int>(stack.size());
  int next = 0;
  if (at == stack.end()) {
    next = direction > 0 ? 0 : count - 1;
  } else {
    next = (static_cast<int>(at - stack.begin()) + (direction > 0 ? 1 : count - 1)) % count;
  }
  placer_.activate(stack[static_cast<size_t>(next)]);
  return true;
}

namespace {

CommandResult ok(const QString& message) { return {true, message}; }
CommandResult fail(const QString& message) { return {false, message}; }

QString directionName(Direction direction) {
  switch (direction) {
    case Direction::Left:
      return QStringLiteral("left");
    case Direction::Right:
      return QStringLiteral("right");
    case Direction::Up:
      return QStringLiteral("up");
    case Direction::Down:
      return QStringLiteral("down");
  }
  return {};
}

}  // namespace

std::optional<TilingEngine::Focused> TilingEngine::focusedManaged() const {
  if (const std::optional<WindowId> front = placer_.foreground()) {
    if (const auto key = model_.keyOf(*front)) return Focused{*key, *front};
  }
  return std::nullopt;
}

std::vector<WindowId> TilingEngine::activeWindows(const WorkspaceKey& key) {
  std::vector<WindowId> active;
  for (const WindowId hwnd : model_.stack(key)) {
    const std::optional<WindowFacts> facts = placer_.state(hwnd);
    if (!facts || !facts->visible || facts->iconic || facts->zoomed || facts->fullscreen ||
        facts->cloaked) {
      continue;
    }
    active.push_back(hwnd);
  }
  return active;
}

CommandResult TilingEngine::focusDirection(Direction direction) {
  if (!running_) return fail(QStringLiteral("the tiler is not running"));
  const std::optional<Focused> focused = focusedManaged();
  if (!focused) return fail(QStringLiteral("the focused window is not tiled"));
  const QRect area = placer_.workArea(focused->key.monitor);
  const std::vector<WindowId> active = activeWindows(focused->key);
  const auto at = std::find(active.begin(), active.end(), focused->hwnd);
  if (area.isEmpty() || at == active.end()) {
    return fail(QStringLiteral("the focused window has no slot"));
  }
  const std::vector<QRect> rects = slotsFor(focused->key, area, active);
  const std::optional<size_t> next =
      neighbourSlot(rects, static_cast<size_t>(at - active.begin()), direction);
  if (!next) return fail(QStringLiteral("no window to the %1").arg(directionName(direction)));
  placer_.activate(active[*next]);
  return ok(QStringLiteral("focused"));
}

CommandResult TilingEngine::moveDirection(Direction direction) {
  if (!running_) return fail(QStringLiteral("the tiler is not running"));
  const std::optional<Focused> focused = focusedManaged();
  if (!focused) return fail(QStringLiteral("the focused window is not tiled"));
  const QRect area = placer_.workArea(focused->key.monitor);
  const std::vector<WindowId> active = activeWindows(focused->key);
  const auto at = std::find(active.begin(), active.end(), focused->hwnd);
  if (area.isEmpty() || at == active.end()) {
    return fail(QStringLiteral("the focused window has no slot"));
  }
  const std::vector<QRect> rects = slotsFor(focused->key, area, active);
  const std::optional<size_t> next =
      neighbourSlot(rects, static_cast<size_t>(at - active.begin()), direction);
  if (!next) return fail(QStringLiteral("no window to the %1").arg(directionName(direction)));
  model_.swap(focused->hwnd, active[*next]);
  requestRetile(focused->key);
  return ok(QStringLiteral("moved"));
}

CommandResult TilingEngine::stackDirection(Direction direction) {
  if (!running_) return fail(QStringLiteral("the tiler is not running"));
  const std::optional<Focused> focused = focusedManaged();
  if (!focused) return fail(QStringLiteral("the focused window is not tiled"));
  const QRect area = placer_.workArea(focused->key.monitor);
  const std::vector<WindowId> active = activeWindows(focused->key);
  const auto at = std::find(active.begin(), active.end(), focused->hwnd);
  if (area.isEmpty() || at == active.end()) {
    return fail(QStringLiteral("the focused window has no slot"));
  }
  const std::vector<QRect> rects = slotsFor(focused->key, area, active);
  const std::optional<size_t> next =
      neighbourSlot(rects, static_cast<size_t>(at - active.begin()), direction);
  if (!next) return fail(QStringLiteral("no window to the %1").arg(directionName(direction)));
  if (!model_.stackOnto(focused->hwnd, active[*next])) {
    return fail(QStringLiteral("cannot stack onto that window"));
  }
  requestRetile(focused->key);
  return ok(QStringLiteral("stacked"));
}

CommandResult TilingEngine::unstack() {
  if (!running_) return fail(QStringLiteral("the tiler is not running"));
  const std::optional<Focused> focused = focusedManaged();
  if (!focused) return fail(QStringLiteral("the focused window is not tiled"));
  if (!model_.unstack(focused->hwnd)) {
    return fail(QStringLiteral("the focused window is not stacked"));
  }
  requestRetile(focused->key);
  return ok(QStringLiteral("unstacked"));
}

CommandResult TilingEngine::cycleStack(int step) {
  if (!running_) return fail(QStringLiteral("the tiler is not running"));
  const std::optional<Focused> focused = focusedManaged();
  if (!focused) return fail(QStringLiteral("the focused window is not tiled"));
  const WindowId top = model_.cycle(focused->hwnd, step);
  if (top == 0) return fail(QStringLiteral("the focused window is not stacked"));
  requestRetile(focused->key);
  placer_.activate(top);
  return ok(QStringLiteral("cycled"));
}

CommandResult TilingEngine::promote() {
  if (!running_) return fail(QStringLiteral("the tiler is not running"));
  const std::optional<Focused> focused = focusedManaged();
  if (!focused) return fail(QStringLiteral("the focused window is not tiled"));
  if (!model_.promote(focused->hwnd)) return ok(QStringLiteral("already the main window"));
  requestRetile(focused->key);
  return ok(QStringLiteral("promoted"));
}

CommandResult TilingEngine::resize(Axis axis, int sign) {
  if (!running_) return fail(QStringLiteral("the tiler is not running"));
  if (sign == 0) return fail(QStringLiteral("no direction"));
  const std::optional<Focused> focused = focusedManaged();
  if (!focused) return fail(QStringLiteral("the focused window is not tiled"));
  const WorkspaceKey& key = focused->key;
  const QString layout = canonicalLayout(model_.layout(key, settings_.layout));
  if (layout == QLatin1String("grid")) {
    return fail(QStringLiteral("the %1 layout does not resize").arg(layout));
  }
  const std::vector<WindowId> active = activeWindows(key);
  const auto at = std::find(active.begin(), active.end(), focused->hwnd);
  if (at == active.end() || active.size() < 2) return ok(QStringLiteral("unchanged"));
  const size_t index = static_cast<size_t>(at - active.begin());
  const bool isMain = index == 0;
  // Along `axis`, the ratio moves or the focused window's weight against its neighbours.
  bool byRatio = false;
  bool byWeight = false;
  const QString shape = resizeShape(layout, active.size());
  if (shape == QLatin1String("columns")) {
    byWeight = axis == Axis::Horizontal;
  } else if (shape == QLatin1String("rows")) {
    byWeight = axis == Axis::Vertical;
  } else if (shape == QLatin1String("centerMain")) {
    // Its side column: window 1, 3, 5... on the right, 2, 4, 6... on the left.
    const size_t others = active.size() - 1;
    const size_t inColumn = isMain ? 0 : ((index - 1) % 2 == 0 ? (others + 1) / 2 : others / 2);
    byRatio = axis == Axis::Horizontal;
    byWeight = axis == Axis::Vertical && inColumn >= 2;
  } else {
    byRatio = axis == Axis::Horizontal;
    byWeight = axis == Axis::Vertical && !isMain && active.size() >= 3;
  }
  if (!byRatio && !byWeight) return ok(QStringLiteral("unchanged"));
  WorkspaceView& view = model_.edit(key);
  if (byRatio) {
    // A window of the stack grows by taking width from the main window, and the other way round.
    const double delta = (isMain ? 1 : -1) * (sign > 0 ? 1 : -1) * kRatioStep;
    const double current = view.ratio.value_or(settings_.ratio);
    const double next = std::clamp(current + delta, 0.1, 0.9);
    if (next == current) return ok(QStringLiteral("unchanged"));
    view.ratio = next;
  } else {
    const auto found = view.weights.find(focused->hwnd);
    const double current = found == view.weights.end() ? 1.0 : found->second;
    const double next =
        std::clamp(current + (sign > 0 ? kWeightStep : -kWeightStep), kMinWeight, kMaxWeight);
    if (next == current) return ok(QStringLiteral("unchanged"));
    view.weights[focused->hwnd] = next;
  }
  requestRetile(key);
  return ok(QStringLiteral("resized"));
}

CommandResult TilingEngine::flip(Axis axis) {
  if (!running_) return fail(QStringLiteral("the tiler is not running"));
  const std::optional<Focused> focused = focusedManaged();
  if (!focused) return fail(QStringLiteral("the focused window is not tiled"));
  WorkspaceView& view = model_.edit(focused->key);
  bool& flag = axis == Axis::Horizontal ? view.flipHorizontal : view.flipVertical;
  flag = !flag;
  requestRetile(focused->key);
  return ok(
      QStringLiteral("flip %1 %2")
          .arg(axis == Axis::Horizontal ? QStringLiteral("horizontal") : QStringLiteral("vertical"),
               flag ? QStringLiteral("on") : QStringLiteral("off")));
}

CommandResult TilingEngine::monocle() {
  if (!running_) return fail(QStringLiteral("the tiler is not running"));
  const std::optional<Focused> focused = focusedManaged();
  if (!focused) return fail(QStringLiteral("the focused window is not tiled"));
  WorkspaceView& view = model_.edit(focused->key);
  const bool off = view.monocle == focused->hwnd;
  view.monocle = off ? 0 : focused->hwnd;
  requestRetile(focused->key);
  return ok(off ? QStringLiteral("monocle off") : QStringLiteral("monocle on"));
}

CommandResult TilingEngine::togglePause() {
  if (!running_) return fail(QStringLiteral("the tiler is not running"));
  paused_ = !paused_;
  if (!paused_) retileAll();
  return ok(paused_ ? QStringLiteral("paused") : QStringLiteral("resumed"));
}

std::optional<WindowId> TilingEngine::actionableForeground(QString* error) {
  const std::optional<WindowId> front = placer_.foreground();
  const std::optional<WindowFacts> facts = front ? placer_.query(*front) : std::nullopt;
  if (!facts || !facts->visible) {
    *error = QStringLiteral("no focused window");
    return std::nullopt;
  }
  if (facts->ownProcess || isBuiltinIgnoredClass(facts->className)) {
    *error = QStringLiteral("the focused window is not an application window");
    return std::nullopt;
  }
  return front;
}

CommandResult TilingEngine::closeForeground() {
  QString error;
  const std::optional<WindowId> hwnd = actionableForeground(&error);
  if (!hwnd) return fail(error);
  return placer_.close(*hwnd) ? ok(QStringLiteral("closing"))
                              : fail(QStringLiteral("close failed"));
}

CommandResult TilingEngine::minimizeForeground() {
  QString error;
  const std::optional<WindowId> hwnd = actionableForeground(&error);
  if (!hwnd) return fail(error);
  return placer_.minimize(*hwnd) ? ok(QStringLiteral("minimized"))
                                 : fail(QStringLiteral("minimize failed"));
}

TilingStatus TilingEngine::status() const {
  TilingStatus out;
  out.running = running_;
  out.desktop = currentDesktop_;
  out.managed = model_.managedCount();
  out.floating = model_.floatingCount();
  out.workspaces = static_cast<int>(model_.keys().size());
  out.stacks = model_.stackCount();
  out.clamped = static_cast<int>(clamped_.size());
  out.layout = currentLayout();
  out.paused = paused_;
  return out;
}

}  // namespace waku::tiling
