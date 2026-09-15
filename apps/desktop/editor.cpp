#include "apps/desktop/editor.hpp"
#include "formats/project/project.hpp"
#include "results/csv.hpp"
#include <QApplication>
#include <QCheckBox>
#include <QCloseEvent>
#include <QComboBox>
#include <QDialog>
#include <QDir>
#include <QDockWidget>
#include <QFileDialog>
#include <QFileInfo>
#include <QFormLayout>
#include <QGraphicsEllipseItem>
#include <QGraphicsPathItem>
#include <QGraphicsScene>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QMenuBar>
#include <QMessageBox>
#include <QPainter>
#include <QPainterPath>
#include <QPainterPathStroker>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QSaveFile>
#include <QSpinBox>
#include <QStandardPaths>
#include <QStatusBar>
#include <QStyle>
#include <QTabWidget>
#include <QTimer>
#include <QToolBar>
#include <QToolButton>
#include <QTreeWidget>
#include <QVBoxLayout>
#include <QtConcurrent/QtConcurrent>
#include <algorithm>
#include <cmath>
#include <limits>
#include <sstream>
namespace pds::desktop {
static QString q(const std::string &s) {
    return QString::fromStdString(s);
}
static std::string serialized(const Project &p) {
    std::ostringstream out;
    write_project(p, out);
    return out.str();
}
static QPointF snapped(QPointF p) {
    return {std::round(p.x() / 20) * 20, std::round(p.y() / 20) * 20};
}
static QString engineering(double value, const std::string &unit) {
    if (value == 0)
        return "0 " + q(unit);
    const std::pair<double, const char *> scales[] = {{1e9, "G"},  {1e6, "M"},  {1e3, "k"},  {1, ""},
                                                      {1e-3, "m"}, {1e-6, "µ"}, {1e-9, "n"}, {1e-12, "p"}};
    for (auto [scale, prefix] : scales)
        if (std::abs(value) >= scale * (1 - 1e-12))
            return QString::number(value / scale, 'g', 8) + " " + QString::fromUtf8(prefix) + q(unit);
    return QString::number(value, 'g', 8) + " " + q(unit);
}
class PortDot final : public QGraphicsItem {
    QColor color_;
    bool hovered_ = false;

  public:
    explicit PortDot(QColor color, QGraphicsItem *parent) : QGraphicsItem(parent), color_(color) {
        setAcceptHoverEvents(true);
        setCursor(Qt::CrossCursor);
        setZValue(5);
    }
    QRectF boundingRect() const override { return {-7, -7, 14, 14}; }
    void paint(QPainter *painter, const QStyleOptionGraphicsItem *, QWidget *) override {
        if (hovered_) {
            painter->setPen(Qt::NoPen);
            auto halo = color_;
            halo.setAlpha(35);
            painter->setBrush(halo);
            painter->drawEllipse(QPointF(), 6, 6);
        }
        painter->setPen(QPen(color_, 1.2));
        painter->setBrush(Qt::white);
        painter->drawEllipse(QPointF(), hovered_ ? 2.8 : 2.0, hovered_ ? 2.8 : 2.0);
    }
    void hoverEnterEvent(QGraphicsSceneHoverEvent *) override {
        hovered_ = true;
        update();
    }
    void hoverLeaveEvent(QGraphicsSceneHoverEvent *) override {
        hovered_ = false;
        update();
    }
};
class WireItem final:public QGraphicsPathItem {
public:QPainterPath shape() const override{QPainterPathStroker stroke;stroke.setWidth(12);return stroke.createStroke(path());}
QRectF boundingRect() const override{return path().boundingRect().adjusted(-6,-6,6,6);}
};
class Atom final : public QGraphicsItem {
  public:
    std::string id;
    QString name, symbol, value;
    int type = 0;
    unsigned input_count = 2;
    bool ground = false;
    Atom(std::string uuid, QString label, QString mark, int category)
        : id(std::move(uuid)), name(label), symbol(mark), type(category) {
        setData(0, q(id));
        setFlags(ItemIsSelectable | ItemIsMovable | ItemSendsGeometryChanges);
        setZValue(2);
    }
    QRectF boundingRect() const override {
        if (type == 3) {
            double h = std::max(36.0, input_count * 12.0);
            return {-80, -h - 14, 160, 2 * h + 52};
        }
        return type == 1 ? QRectF(-46, -12, 92, 68) : QRectF(-78, -53, 156, 104);
    }
    void port(const QString &port_name, QPointF location, const QColor &color) {
        auto *item = new PortDot(color, this);
        item->setPos(location);
        item->setData(0, q(id));
        item->setData(1, "port");
        item->setData(2, port_name);
        item->setToolTip(port_name);
    }
    QVariant itemChange(GraphicsItemChange change, const QVariant &proposed) override {
        if (change == ItemPositionChange)
            return snapped(proposed.toPointF());
        return QGraphicsItem::itemChange(change, proposed);
    }
    static void label(QPainter* p,QRectF rect,int flags,const QString& value){
        auto t=p->worldTransform();auto center=t.map(rect.center());double scale=std::sqrt(std::abs(t.determinant()));p->save();p->resetTransform();p->translate(center);p->scale(scale,scale);p->drawText(QRectF(-rect.width()/2,-rect.height()/2,rect.width(),rect.height()),flags,value);p->restore();
    }
    void paint(QPainter *p, const QStyleOptionGraphicsItem *, QWidget *) override {
        p->setPen(QPen(isSelected() ? QColor("#e88b22") : QColor("#263c55"), 2));
        p->setBrush(Qt::white);
        if (type == 3) {
            double h = std::max(36.0, input_count * 12.0);
            p->setBrush(QColor("#ffffff"));
            p->setPen(QPen(isSelected() ? QColor("#3a7fe0") : QColor("#a7b8ce"), 1.5));
            p->drawRoundedRect(QRectF(-46, -h, 104, 2 * h), 7, 7);
            p->setPen(QPen(QColor("#c7d5e8"), 1));
            p->drawLine(-24, 17, -24, -17);
            p->drawLine(-24, 17, 37, 17);
            p->setPen(QPen(QColor("#5469d4"), 2));
            QPainterPath curve;
            curve.moveTo(-20, 12);
            curve.cubicTo(-6, 10, -4, -15, 10, -11);
            curve.cubicTo(25, -8, 20, 7, 36, -4);
            p->drawPath(curve);
            for (unsigned i = 1; i <= input_count; ++i) {
                double y = (static_cast<double>(i) - (input_count + 1) / 2.0) * 22;
                p->setPen(QPen(QColor("#8c67c8"), 1.4));
                p->drawLine(QPointF(-70, y), QPointF(-46, y));
                label(p,QRectF(-43, y - 9, 16, 18), Qt::AlignCenter, QString::number(i));
            }
            p->setPen(QColor("#334c69"));
            label(p,QRectF(-78, h + 7, 156, 24), Qt::AlignCenter, name);
            return;
        }
        if (type == 1) {
            if (ground) {
                p->drawLine(0, 0, 0, 15);
                p->drawLine(-16, 15, 16, 15);
                p->drawLine(-10, 21, 10, 21);
                p->drawLine(-4, 27, 4, 27);
            } else {
                p->setBrush(QColor("#263c55"));
                p->drawEllipse(QPointF(0, 0), 4, 4);
            }
            label(p,QRectF(-45, 31, 90, 20), Qt::AlignCenter, name);
            return;
        }
        if (type == 2) {
            p->setBrush(QColor("#e6f5ed"));
            p->drawRoundedRect(QRectF(-38, -22, 76, 44), 7, 7);
            label(p,QRectF(-38, -22, 76, 44), Qt::AlignCenter, symbol.isEmpty()?QString("Gate"):symbol);
            p->drawLine(38, 0, 60, 0);
        } else {
            p->drawLine(-60, 0, -27, 0);
            p->drawLine(27, 0, 60, 0);
            p->save();
            auto port_font = p->font();
            port_font.setPointSize(8);
            p->setFont(port_font);
            label(p,QRectF(-61, 4, 18, 16), Qt::AlignLeft, "p");
            label(p,QRectF(44, 4, 18, 16), Qt::AlignRight, "n");
            p->restore();
            if (symbol == "R")
                p->drawRect(QRectF(-27, -12, 54, 24));
            else if (symbol == "C") {
                p->drawLine(-27, 0, -7, 0);
                p->drawLine(7, 0, 27, 0);
                p->drawLine(-7, -22, -7, 22);
                p->drawLine(7, -22, 7, 22);
            } else if (symbol == "L") {
                for (int i = 0; i < 4; ++i)
                    p->drawArc(QRectF(-28 + i * 14, -14, 14, 28), 0, 180 * 16);
            } else if (symbol == "S") {
                p->drawLine(-27, 0, 20, -18);
                p->drawLine(0, -40, 0, -26);
            } else if (symbol == "D") {
                QPolygonF triangle;
                triangle << QPointF(-20, -18) << QPointF(-20, 18) << QPointF(20, 0);
                p->drawPolygon(triangle);
                p->drawLine(20, -18, 20, 18);
                p->drawLine(-27, 0, -20, 0);
                p->drawLine(20, 0, 27, 0);
            } else {
                p->drawEllipse(QRectF(-27, -27, 54, 54));
                label(p,QRectF(-27, -27, 54, 54), Qt::AlignCenter, symbol);
            }
        }
        label(p,QRectF(-77, 28, 154, 22), Qt::AlignCenter, name);
        if (!value.isEmpty())
            label(p,QRectF(-77, -51, 154, 20), Qt::AlignCenter, value);
    }
};
void EditorWindow::set_placement_preview(){
    Atom* a=nullptr;
    if(placing_<100){a=new Atom("","",q(kind_name(static_cast<Kind>(placing_))),0);a->port("p",{-60,0},QColor("#146cca"));a->port("n",{60,0},QColor("#146cca"));}
    else {int type=placing_==103?3:(placing_==102||placing_==104?2:1);a=new Atom("","",placing_==104?QString("PWM"):QString(),type);a->ground=placing_==100;}
    canvas_->set_ghost(a);
}
EditorWindow::EditorWindow(const QString &language, const QString &recovery_dir)
    : recovery_dir_(recovery_dir) {
    init_language(language);
    auto ui_font = font();
    ui_font.setPointSize(10);
    setFont(ui_font);
    resize(1360, 900);
    setMinimumSize(1050, 720);
    if (recovery_dir_.isEmpty())
        recovery_dir_ = QStandardPaths::writableLocation(QStandardPaths::AppLocalDataLocation);
    build_ui();
    connect(&watcher_, &QFutureWatcher<Outcome>::finished, this, [this] { finish_simulation(); });
    auto *timer = new QTimer(this);
    connect(timer, &QTimer::timeout, this, [this] { autosave(); });
    timer->start(15000);
    auto *progress_timer = new QTimer(this);
    connect(progress_timer, &QTimer::timeout, this, [this] {
        if (running() && !cancel_.load())
            banner_->setText(text("running") + "  t = " +
                             QString::number(simulated_time_.load(std::memory_order_relaxed), 'g', 6) +
                             " / " + QString::number(project().profile.stop, 'g', 6) + " s");
    });
    progress_timer->start(100);
    new_file();
    if (QFile::exists(recovery_path()))
        banner_->setText(text("recovery_available"));
}
EditorWindow::~EditorWindow() {
    rebuilding_ = true;
    disconnect(canvas_->scene(), nullptr, this, nullptr);
    disconnect(&watcher_, nullptr, this, nullptr);
    cancel_ = true;
    watcher_.waitForFinished();
}
void EditorWindow::build_ui() {
    setStyleSheet(R"(
        QMainWindow,QDialog { background:#f3f6fa; color:#25344a; }
        QMenuBar { background:#ffffff; padding:4px 12px; border-bottom:1px solid #dfe6ef; }
        QMenuBar::item { padding:5px 12px; } QMenuBar::item:selected { background:#e8f0ff; border-radius:4px; }
        QToolBar#controls { background:#182c47; padding:12px 16px; spacing:10px; border:0; }
        QToolBar#controls QLabel { color:#dce7f7; } QToolBar#controls QLineEdit,QToolBar#controls QComboBox { background:#29415f; color:white; border:1px solid #49617c; border-radius:5px; padding:6px 8px; }
        QToolBar#controls QComboBox QAbstractItemView { background:white; color:#25344a; }
        QToolButton { padding:7px 12px; border:1px solid #dbe3ee; border-radius:5px; background:white; color:#25344a; }
        QToolButton:hover { background:#edf4ff; border-color:#a8c3ec; }
        QToolButton#run_button { background:#16a085; color:white; border:0; padding:9px 24px; font-weight:600; }
        QToolButton#run_button:hover { background:#118873; } QToolButton:disabled { color:#94a3b8; background:#e9eef5; }
        QToolButton#stop_button { background:#29415f; color:#dce7f7; border:1px solid #49617c; }
        QDockWidget::title { background:#f3f6fa; color:#52677e; padding:9px 12px; font-weight:600; }
        QLineEdit,QPlainTextEdit,QComboBox,QSpinBox { background:white; border:1px solid #d6e0ed; border-radius:5px; padding:7px; selection-background-color:#2c71d9; }
        QLineEdit:focus,QPlainTextEdit:focus,QSpinBox:focus { border-color:#4388e5; }
        QTreeWidget,QListWidget { background:white; border:0; outline:0; padding:4px; }
        QTreeWidget::item,QListWidget::item { min-height:28px; border-radius:4px; padding:2px 6px; }
        QTreeWidget::item:selected,QListWidget::item:selected { background:#e6f0ff; color:#194f98; }
        QTreeWidget::item:hover,QListWidget::item:hover { background:#f1f6fd; }
        QPushButton { background:white; border:1px solid #cfdceb; border-radius:5px; padding:8px 12px; color:#2f527c; }
        QPushButton:hover { background:#eaf3ff; } QPushButton#apply_properties { background:#e7f0fd; border-color:#c5daf7; color:#225693; font-weight:600; }
        QTabWidget::pane { border:1px solid #dce5ef; background:white; }
        QTabBar::tab { background:#edf2f8; padding:10px 18px; border:0; color:#718299; }
        QTabBar::tab:selected { background:white; color:#24558e; border-top:2px solid #3a7fe0; }
        QCheckBox { spacing:8px; padding:4px; } QCheckBox::indicator { width:15px; height:15px; }
        QStatusBar { background:white; color:#6b7f95; border-top:1px solid #dfe6ef; padding:4px 12px; }
    )");
    auto *file_menu = menuBar()->addMenu(text("file_menu"));
    auto *edit_menu = menuBar()->addMenu(text("edit_menu"));
    auto *view_menu = menuBar()->addMenu(text("view_menu"));
    auto action = [&](QMenu *menu, const char *key, const QKeySequence &shortcut, auto callback) {
        auto *a = new QAction(text(key), this);
        a->setObjectName(QString("action_") + key);
        a->setShortcut(shortcut);
        commands_[key]=a;default_shortcuts_[key]=shortcut;
        addAction(a);
        if (menu)
            menu->addAction(a);
        connect(a, &QAction::triggered, this, callback);
        return a;
    };
    action(file_menu, "new", QKeySequence::New, [this] {
        if (!running() && confirm_discard())
            new_file();
    });
    action(file_menu, "open", QKeySequence::Open, [this] {
        if (running() || !confirm_discard())
            return;
        auto f = QFileDialog::getOpenFileName(this, text("open"), {}, text("project_filter"));
        if (!f.isEmpty())
            open_project(f);
    });
    action(file_menu, "save", QKeySequence::Save, [this] {
        auto f = path_;
        if (f.isEmpty())
            f = QFileDialog::getSaveFileName(this, text("save"), {}, text("project_filter"));
        if (!f.isEmpty())
            save_project(f);
    });
    file_menu->addSeparator();
    action(file_menu, "recover", {}, [this] {
        if (running() || !confirm_discard())
            return;
        if (QFile::exists(recovery_path()))
            recover(recovery_path());
        else
            banner_->setText(text("no_recovery"));
    });
    undo_ = action(edit_menu, "undo", QKeySequence::Undo, [this] { undo(); });
    redo_ = action(edit_menu, "redo", QKeySequence::Redo, [this] { redo(); });
    action(edit_menu, "delete", QKeySequence::Delete, [this] { delete_selected(); });
    action(edit_menu,"rotate",QKeySequence("Space"),[this]{transform_selection(1,false);});
    action(edit_menu,"rotate_back",QKeySequence("Shift+Space"),[this]{transform_selection(-1,false);});
    action(edit_menu,"mirror",QKeySequence("Ctrl+M"),[this]{transform_selection(0,true);});
    action(edit_menu,"copy",QKeySequence::Copy,[this]{copy_selection(false);});
    action(edit_menu,"cut",QKeySequence::Cut,[this]{copy_selection(true);});
    action(edit_menu,"paste",QKeySequence::Paste,[this]{paste_selection();});
    action(edit_menu,"duplicate",QKeySequence("Ctrl+D"),[this]{paste_selection(true);});
    action(edit_menu,"select_all",QKeySequence::SelectAll,[this]{for(auto& [id,item]:atoms_)item->setSelected(true);});
    action(edit_menu,"properties",QKeySequence("Alt+Return"),[this]{fill_inspector();name_->setFocus();name_->selectAll();});
    for(const char* mode:{"left","right","top","bottom","horizontal","vertical"})action(nullptr,mode,{},[this,mode]{arrange_selection(mode);});
    action(edit_menu,"shortcuts",{},[this]{show_shortcuts();});
    auto *wire_action = action(edit_menu, "connect_tool", QKeySequence("Ctrl+W"), [this] {
        if (running())
            return;
        placing_ = 101;set_placement_preview();banner_->setText(text("junction_hint"));
    });
    auto *fit_action = action(view_menu, "fit", QKeySequence("F"), [this] {
        canvas_->fitInView(canvas_->scene()->itemsBoundingRect().adjusted(-90, -90, 90, 90),
                           Qt::KeepAspectRatio);
        if (scope_)
            scope_->fit();
    });
    action(view_menu, "diagnostics", {}, [this] { bottom_->setCurrentIndex(0); });
    action(view_menu, "scope_tab", {}, [this] { bottom_->setCurrentIndex(1); });
    auto *examples = menuBar()->addMenu(text("examples"));
    QDir dir(QCoreApplication::applicationDirPath() + "/examples");
    for (const auto &file : dir.entryList({"*.pds"}, QDir::Files))
        connect(examples->addAction(file), &QAction::triggered, this, [this, dir, file] {
            if (!running() && confirm_discard())
                open_project(dir.filePath(file));
        });
    auto *toolbar = addToolBar("PowerDriveSim");
    toolbar->setObjectName("controls");
    toolbar->setMovable(false);
    auto *brand = new QLabel("∿  PowerDriveSim");
    brand->setStyleSheet("font-size:17px;font-weight:600;color:white;padding-right:20px;");
    toolbar->addWidget(brand);
    toolbar->addWidget(new QLabel(text("duration")));
    stop_ = new QLineEdit;
    stop_->setObjectName("sim_stop");
    stop_->setFixedWidth(90);
    toolbar->addWidget(stop_);
    toolbar->addWidget(new QLabel(text("step_short")));
    step_ = new QLineEdit;
    step_->setObjectName("sim_step");
    step_->setFixedWidth(90);
    toolbar->addWidget(step_);
    method_ = new QComboBox;
    method_->setObjectName("sim_method");
    method_->addItems({"Backward Euler", "Trapezoidal"});
    method_->setToolTip(text("method"));
    method_->setFixedWidth(150);
    toolbar->addWidget(method_);
    run_ = action(nullptr, "run", QKeySequence("Ctrl+R"), [this] { start_simulation(); });
    auto *run_button = new QToolButton;
    run_button->setObjectName("run_button");
    run_button->setDefaultAction(run_);
    toolbar->addWidget(run_button);
    stop_action_ = action(nullptr, "stop", QKeySequence("Escape"), [this] { stop_simulation(); });
    auto *stop_button = new QToolButton;
    stop_button->setObjectName("stop_button");
    stop_button->setDefaultAction(stop_action_);
    toolbar->addWidget(stop_button);
    auto *spacer = new QWidget;
    spacer->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Preferred);
    toolbar->addWidget(spacer);
    auto *center = new QWidget;
    auto *layout = new QVBoxLayout(center);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(0);
    auto *canvas_tools = new QWidget;
    canvas_tools->setStyleSheet("background:white;border-bottom:1px solid #dfe6ef;");
    auto *tools_layout = new QHBoxLayout(canvas_tools);
    tools_layout->setContentsMargins(16, 8, 16, 8);
    auto *title = new QLabel(text("canvas_title"));
    title->setStyleSheet("font-weight:600;border:0;padding-right:16px;");
    tools_layout->addWidget(title);
    tools_layout->addStretch();
    for (auto *a : {wire_action, undo_, redo_, fit_action}) {
        auto *button = new QToolButton;
        button->setDefaultAction(a);
        tools_layout->addWidget(button);
    }
    layout->addWidget(canvas_tools);
    canvas_ = new Canvas;
    layout->addWidget(canvas_, 1);
    banner_ = new QLabel(text("hint"));
    banner_->setStyleSheet("padding:9px 16px;color:#657c95;background:#f8fafc;border-top:1px solid #e2e9f1;");
    banner_->setWordWrap(true);
    layout->addWidget(banner_);
    setCentralWidget(center);
    auto dock = [&](const char *key, QWidget *widget, Qt::DockWidgetArea area) {
        auto *d = new QDockWidget(text(key), this);
        d->setObjectName(key);
        d->setFeatures(QDockWidget::NoDockWidgetFeatures);
        d->setWidget(widget);
        addDockWidget(area, d);
        return d;
    };
    auto *left_tabs = new QTabWidget;
    auto *lib = new QWidget;
    auto *ll = new QVBoxLayout(lib);
    ll->setContentsMargins(10, 12, 10, 8);
    ll->setSpacing(12);
    auto *search = new QLineEdit;
    search->setPlaceholderText(text("search"));
    ll->addWidget(search);
    library_ = new QTreeWidget;
    library_->setObjectName("library");
    library_->setHeaderHidden(true);
    library_->setIndentation(16);
    library_->setRootIsDecorated(true);
    library_->setUniformRowHeights(true);
    ll->addWidget(library_);
    auto category = [&](const char *name, std::initializer_list<std::pair<const char *, int>> entries) {
        auto *parent = new QTreeWidgetItem(library_, {text(name)});
        auto font = parent->font(0);
        font.setBold(true);
        parent->setFont(0, font);
        parent->setFlags(Qt::ItemIsEnabled);
        for (auto entry : entries) {
            auto *item = new QTreeWidgetItem(parent, {text(entry.first)});
            item->setData(0, Qt::UserRole, entry.second);
        }
        parent->setExpanded(true);
    };
    category("passive", {{"resistor", 0}, {"capacitor", 1}, {"inductor", 2}});
    category("sources", {{"voltage_source", 3}, {"current_source", 4}, {"ground", 100}});
    category("switching", {{"switch", 5}, {"diode", 6}});
    category("signals", {{"gate_pattern", 102}, {"pwm",104}});
    category("measurements", {{"voltage_probe", 7}, {"current_probe", 8}, {"plot", 103}});
    connect(search, &QLineEdit::textChanged, this, [this](const QString &query) {
        for (int i = 0; i < library_->topLevelItemCount(); ++i) {
            auto *parent = library_->topLevelItem(i);
            bool any = false;
            for (int j = 0; j < parent->childCount(); ++j) {
                auto *child = parent->child(j);
                bool match = child->text(0).contains(query, Qt::CaseInsensitive) ||
                             parent->text(0).contains(query, Qt::CaseInsensitive);
                child->setHidden(!match);
                any |= match;
            }
            parent->setHidden(!any);
            if (!query.isEmpty())
                parent->setExpanded(true);
        }
    });
    connect(library_, &QTreeWidget::itemDoubleClicked, this, [this](QTreeWidgetItem *item, int) {
        if (!item->data(0, Qt::UserRole).isValid())
            return;
        placing_ = item->data(0, Qt::UserRole).toInt();
        set_placement_preview();
        pending_port_.reset();
        banner_->setText(text("place_hint"));
    });
    left_tabs->addTab(lib, text("library"));
    objects_ = new QListWidget;
    connect(objects_, &QListWidget::itemClicked, this,
            [this](QListWidgetItem *i) { select_object(i->data(Qt::UserRole).toString().toStdString()); });
    left_tabs->addTab(objects_, text("objects"));
    auto *left = dock("workspace", left_tabs, Qt::LeftDockWidgetArea);
    left->setMinimumWidth(255);
    left->setMaximumWidth(380);
    auto *inspector = new QWidget;
    properties_ = new QFormLayout(inspector);
    properties_->setContentsMargins(16, 18, 16, 18);
    properties_->setVerticalSpacing(14);
    inspector_hint_ = new QLabel(text("inspector_empty"));
    inspector_hint_->setWordWrap(true);
    inspector_hint_->setStyleSheet("color:#8191a6;padding:18px 0;");
    properties_->addRow(inspector_hint_);
    properties_->setFieldGrowthPolicy(QFormLayout::AllNonFixedFieldsGrow);
    auto line = [&](const char *label, const char *id) {
        auto *w = new QLineEdit;
        w->setObjectName(id);
        properties_->addRow(text(label), w);
        return w;
    };
    name_ = line("name", "property_name");
    value_ = line("value", "property_value");
    initial_ = line("initial", "property_initial");
    frequency_=line("frequency","property_frequency");duty_=line("duty","property_duty");delay_=line("delay","property_delay");
    inputs_ = new QSpinBox;
    inputs_->setRange(1, 16);
    inputs_->setObjectName("property_inputs");
    properties_->addRow(text("plot_inputs"), inputs_);
    closed_ = new QCheckBox;
    closed_->setObjectName("property_closed");
    properties_->addRow(text("closed"), closed_);
    events_ = new QPlainTextEdit;
    events_->setObjectName("property_events");
    events_->setPlaceholderText("1ms 1\n2ms 0");
    events_->setMaximumHeight(120);
    properties_->addRow(text("events"), events_);
    bends_ = new QPlainTextEdit;
    bends_->setObjectName("property_bends");
    bends_->setPlaceholderText("x y");
    bends_->setMaximumHeight(100);
    properties_->addRow(text("bends"), bends_);
    auto *apply = new QPushButton(text("apply"));
    apply->setObjectName("apply_properties");
    apply_button_ = apply;
    properties_->addRow(apply);
    connect(apply, &QPushButton::clicked, this, [this] { apply_inspector(); });
    auto *right = dock("inspector", inspector, Qt::RightDockWidgetArea);
    right->setMinimumWidth(280);
    right->setMaximumWidth(400);
    bottom_ = new QTabWidget;
    bottom_->setObjectName("results_tabs");
    auto *diagnostics = new QWidget;
    auto *dl = new QVBoxLayout(diagnostics);
    auto *diagnostic_hint = new QLabel(text("diagnostics_hint"));
    diagnostic_hint->setObjectName("diagnostic_status");
    diagnostic_hint->setStyleSheet("padding:5px 12px;color:#72869c;");
    dl->addWidget(diagnostic_hint);
    errors_ = new QListWidget;
    errors_->setObjectName("diagnostics_list");
    dl->addWidget(errors_, 1);
    bottom_->addTab(diagnostics, text("diagnostics"));
    connect(errors_, &QListWidget::itemClicked, this,
            [this](QListWidgetItem *i) { select_object(i->data(Qt::UserRole).toString().toStdString()); });
    scope_page_ = new QWidget;
    scope_layout_ = new QVBoxLayout(scope_page_);
    scope_layout_->setContentsMargins(12, 12, 12, 12);
    auto *scope_header = new QHBoxLayout;
    scope_enable_ = new QCheckBox(text("scope_record"));
    scope_enable_->setObjectName("scope_enable");
    scope_header->addWidget(scope_enable_);
    scope_header->addStretch();
    scope_export_ = new QPushButton(text("export_selected"));
    scope_export_->setEnabled(false);
    scope_header->addWidget(scope_export_);
    scope_layout_->addLayout(scope_header);
    scope_hint_ = new QLabel(text("scope_disabled"));
    scope_hint_->setWordWrap(true);
    scope_hint_->setAlignment(Qt::AlignCenter);
    scope_hint_->setStyleSheet("color:#8090a4;padding:24px;font-size:13px;");
    scope_layout_->addWidget(scope_hint_, 1);
    bottom_->addTab(scope_page_, text("scope_tab"));
    connect(scope_enable_, &QCheckBox::toggled, this, [this](bool checked) {
        if (!rebuilding_ && document_)
            set_scope_enabled(checked);
    });
    connect(scope_export_, &QPushButton::clicked, this, [this] { export_csv(project().scope_channels); });
    auto *bottom_dock = dock("results", bottom_, Qt::BottomDockWidgetArea);
    bottom_dock->setTitleBarWidget(new QWidget);
    bottom_dock->setMinimumHeight(220);
    canvas_->place = [this](QPointF point) {
        if (running())
            return;
        if (pending_port_) {
            auto from = *pending_port_;
            auto id = new_uuid();
            point = snapped(point);
            try {
                document_->apply("Continue wire", [&](Project &p) {
                    p.nodes.push_back(
                        {id, "N" + std::to_string(p.nodes.size() + 1), false, point.x(), point.y()});
                    p.wires.push_back({new_uuid(), from, {id, "node"}, {}});
                });
                selected_ = id;
                pending_port_ = Endpoint{id, "node"};
                refresh();
                banner_->setText(text("wire_hint"));
            } catch (const std::exception &e) {
                show_error(e);
            }
            return;
        }
        if (placing_ < 0)
            return;
        int kind = placing_;
        placing_ = -1;canvas_->set_ghost(nullptr);
        if (kind < 100)
            add_component(static_cast<Kind>(kind), point);
        else if (kind == 102)
            add_pattern(point);
        else if(kind==104){auto id=new_uuid();point=snapped(point);document_->apply("Add PWM",[&](Project& p){GatePattern g{id,text("pwm").toStdString(),point.x(),point.y()};g.pwm=true;p.patterns.push_back(g);});selected_=id;refresh();}
        else if (kind == 103)
            add_plot(point);
        else
            add_node(kind == 100, point);
        library_->clearSelection();
        banner_->setText(text("hint"));
    };
    canvas_->wire_dropped=[this](Endpoint from,std::optional<Endpoint> to,QPointF point,std::string wire){finish_wire(from,to,point,wire);};
    canvas_->compatible=[this](const Endpoint& a,const Endpoint& b){try{validate_wire(project(),Wire{"",a,b,{}});return true;}catch(...){return false;}};
    canvas_->context_menu=[this](std::string id,QPoint point){show_context(id,point);};
    load_shortcuts();
    canvas_->port_clicked = [this](Endpoint e) { handle_port(std::move(e)); };
    canvas_->movement = [this] { update_wires(); };
    canvas_->released = [this] { commit_positions(); };
    canvas_->open_object = [this](std::string id) { open_plot(id); };
    canvas_->observe = [this](std::string id) { observe_object(id); };
    connect(canvas_->scene(), &QGraphicsScene::selectionChanged, this, [this] {
        if (rebuilding_)
            return;
        auto items = canvas_->scene()->selectedItems();
        selected_ = items.empty() ? "" : items.front()->data(0).toString().toStdString();
        fill_inspector();
        update_wires();
    });
}
void EditorWindow::set_project(Project p) {
    for (auto &[id, window] : plot_windows_) {
        (void)id;
        if (window)
            delete window;
    }
    plot_windows_.clear();
    plot_views_.clear();
    document_ = std::make_unique<Document>(std::move(p));
    selected_.clear();
    pending_port_.reset();
    path_.clear();
    saved_state_ = serialized(project());
    refresh();
    canvas_->resetTransform();
    canvas_->centerOn(200, 120);
    if (!project().components.empty())
        canvas_->fitInView(canvas_->scene()->itemsBoundingRect().adjusted(-80, -80, 80, 80),
                           Qt::KeepAspectRatio);
}
void EditorWindow::new_file() {
    Project p;
    p.id = new_uuid();
    p.name = text("untitled").toStdString();
    p.wired = true;
    set_project(std::move(p));
    banner_->setText(text("hint"));
}
bool EditorWindow::open_project(const QString &path) {
    try {
        QFile file(path);
        if (!file.open(QIODevice::ReadOnly))
            throw std::runtime_error(file.errorString().toStdString());
        std::istringstream in(file.readAll().toStdString());
        set_project(read_project(in));
        path_ = path;
        update_title();
        banner_->setText(text("hint"));
        return true;
    } catch (const std::exception &e) {
        show_error(e);
        return false;
    }
}
bool EditorWindow::save_project(const QString &path) {
    try {
        QSaveFile file(path);
        auto bytes = serialized(project());
        if (!file.open(QIODevice::WriteOnly) ||
            file.write(bytes.data(), static_cast<qint64>(bytes.size())) !=
                static_cast<qint64>(bytes.size()) ||
            !file.commit())
            throw std::runtime_error(file.errorString().toStdString());
        path_ = path;
        saved_state_ = bytes;
        update_title();
        banner_->setText(text("saved"));
        return true;
    } catch (const std::exception &e) {
        show_error(e);
        return false;
    }
}
QString EditorWindow::recovery_path() const {
    return recovery_dir_ + "/recovery.pds.autosave";
}
bool EditorWindow::autosave() {
    if (!document_ || serialized(project()) == saved_state_)
        return false;
    QDir().mkpath(recovery_dir_);
    QSaveFile file(recovery_path());
    auto bytes = serialized(project());
    return file.open(QIODevice::WriteOnly) &&
           file.write(bytes.data(), static_cast<qint64>(bytes.size())) == static_cast<qint64>(bytes.size()) &&
           file.commit();
}
bool EditorWindow::recover(const QString &path) {
    if (!open_project(path))
        return false;
    path_.clear();
    saved_state_.clear();
    update_title();
    banner_->setText(text("recovered"));
    return true;
}
void EditorWindow::refresh(bool invalidate) {
    rebuilding_ = true;
    if (invalidate)
        clear_result();
    rebuild_scene();
    objects_->clear();
    auto item = [&](const std::string &id, const std::string &name) {
        auto *i = new QListWidgetItem(q(name), objects_);
        i->setData(Qt::UserRole, q(id));
    };
    for (const auto &c : project().components)
        item(c.id, c.name);
    for (const auto &n : project().nodes)
        item(n.id, n.name);
    for (const auto &g : project().patterns)
        item(g.id, g.name);
    for (const auto &g : project().plots)
        item(g.id, g.name);
    stop_->setText(QString::number(project().profile.stop, 'g', 12));
    step_->setText(QString::number(project().profile.step, 'g', 12));
    method_->setCurrentIndex(project().profile.method == Method::trapezoidal ? 1 : 0);
    undo_->setEnabled(document_->can_undo() && !running());
    redo_->setEnabled(document_->can_redo() && !running());
    sync_scope();
    refresh_channel_catalog();
    update_graphs();
    rebuilding_ = false;
    fill_inspector();
    update_title();
}
void EditorWindow::rebuild_scene() {
    canvas_->cancel_wire();canvas_->set_ghost(nullptr);
    atoms_.clear();
    wires_.clear();
    canvas_->scene()->clear();
    for (const auto &c : project().components) {
        auto *a = new Atom(c.id, q(c.name), q(kind_name(c.kind)), 0);
        auto unit = component_unit(c.kind);
        a->value = unit.empty() ? QString() : engineering(c.value, unit);
        a->port("p", {-60, 0}, QColor("#146cca"));
        a->port("n", {60, 0}, QColor("#146cca"));
        if (c.kind == Kind::ideal_switch)
            a->port("gate", {0, -40}, QColor("#17866d"));
        if (c.kind == Kind::voltage_probe || c.kind == Kind::current_probe)
            a->port("out", {0, -40}, QColor("#935ad5"));
        canvas_->scene()->addItem(a);
        a->setFlag(QGraphicsItem::ItemSendsGeometryChanges, false);
        a->setPos(c.x, c.y);
        a->setFlag(QGraphicsItem::ItemSendsGeometryChanges, true);
        atoms_[c.id] = a;
    }
    for (const auto &n : project().nodes) {
        auto *a = new Atom(n.id, q(n.name), {}, 1);
        a->ground = n.ground;
        a->port("node", {0, 0}, QColor("#146cca"));
        canvas_->scene()->addItem(a);
        a->setFlag(QGraphicsItem::ItemSendsGeometryChanges, false);
        a->setPos(n.x, n.y);
        a->setFlag(QGraphicsItem::ItemSendsGeometryChanges, true);
        atoms_[n.id] = a;
    }
    for (const auto &g : project().patterns) {
        auto *a = new Atom(g.id, q(g.name), g.pwm?QString("PWM"):QString(), 2);
        if(g.pwm)a->value=QString::number(g.frequency)+" Hz · "+QString::number(g.duty*100)+" %";
        a->port("out", {60, 0}, QColor("#17866d"));
        canvas_->scene()->addItem(a);
        a->setFlag(QGraphicsItem::ItemSendsGeometryChanges, false);
        a->setPos(g.x, g.y);
        a->setFlag(QGraphicsItem::ItemSendsGeometryChanges, true);
        atoms_[g.id] = a;
    }
    for (const auto &g : project().plots) {
        auto *a = new Atom(g.id, q(g.name), {}, 3);
        a->input_count = g.inputs;
        for (unsigned i = 1; i <= g.inputs; ++i)
            a->port("in" + QString::number(i), {-70, (static_cast<double>(i) - (g.inputs + 1) / 2.0) * 22},
                    QColor("#8c67c8"));
        canvas_->scene()->addItem(a);
        a->setFlag(QGraphicsItem::ItemSendsGeometryChanges, false);
        a->setPos(g.x, g.y);
        a->setFlag(QGraphicsItem::ItemSendsGeometryChanges, true);
        atoms_[g.id] = a;
    }
    auto orient=[&](const auto& objects){for(const auto& o:objects){int c[]={1,0,-1,0},sn[]={0,1,0,-1};unsigned t=o.orientation.quarter_turns%4;double sign=o.orientation.mirrored?-1:1;atoms_.at(o.id)->setTransform(QTransform(sign*c[t],sn[t],-sign*sn[t],c[t],0,0));}};
    orient(project().components);orient(project().nodes);orient(project().patterns);orient(project().plots);
    for (const auto &w : project().wires) {
        auto *item = new WireItem;
        item->setFlag(QGraphicsItem::ItemIsSelectable);
        item->setData(0, q(w.id));
        item->setData(1,"wire");
        item->setZValue(-2);
        canvas_->scene()->addItem(item);
        wires_[w.id] = item;
    }
    if (atoms_.count(selected_))
        atoms_.at(selected_)->setSelected(true);
    if (wires_.count(selected_))
        wires_.at(selected_)->setSelected(true);
    canvas_->setSceneRect(-10000, -10000, 20000, 20000);
    update_wires();
}
QPointF EditorWindow::port_position(const Endpoint &e) const {
    auto it = atoms_.find(e.object);
    if (it == atoms_.end())
        return {};
    for (auto *child : it->second->childItems())
        if (child->data(2).toString() == q(e.port))
            return child->scenePos();
    return {};
}
void EditorWindow::update_wires() {
    if (!document_)
        return;
    const auto graph = resolve_connections(project());
    std::string highlight;
    if (pending_port_) {
        auto i = graph.nets.find(endpoint_key(*pending_port_));
        if (i != graph.nets.end())
            highlight = i->second;
    }
    for (const auto &w : project().wires)
        if (w.id == selected_) {
            auto i = graph.nets.find(endpoint_key(w.from));
            if (i != graph.nets.end())
                highlight = i->second;
        }
    std::vector<QRectF> obstacles;
    for (const auto &[id, atom] : atoms_) {
        (void)id;
        auto *shape = static_cast<Atom *>(atom);
        if (shape->type == 3) {
            double h = std::max(36.0, shape->input_count * 12.0);
            obstacles.push_back(atom->mapRectToScene(QRectF(-46, -h,104,2*h)));
        } else if (shape->type != 1)
            obstacles.push_back(atom->mapRectToScene(QRectF(-38,-28,76,56)));
    }
    auto stub = [&](const Endpoint &e, QPointF point) {
        QPointF delta(0,-20);
        if(e.port=="p"||e.port.rfind("in",0)==0)delta={-20,0};
        else if(e.port=="n"||(e.port=="out"&&static_cast<Atom*>(atoms_.at(e.object))->type==2))delta={20,0};
        auto* atom=atoms_.at(e.object);return point+atom->mapToScene(delta)-atom->mapToScene(QPointF());
    };
    auto clear = [&](const std::vector<QPointF> &route) {
        for (size_t i = 1; i < route.size(); ++i)
            for (const auto &box : obstacles) {
                auto a = route[i - 1], b = route[i];
                if (a.x() == b.x() && a.x() > box.left() && a.x() < box.right() &&
                    std::max(a.y(), b.y()) > box.top() && std::min(a.y(), b.y()) < box.bottom())
                    return false;
                if (a.y() == b.y() && a.y() > box.top() && a.y() < box.bottom() &&
                    std::max(a.x(), b.x()) > box.left() && std::min(a.x(), b.x()) < box.right())
                    return false;
            }
        return true;
    };
    for (const auto &w : project().wires) {
        auto found = wires_.find(w.id);
        if (found == wires_.end())
            continue;
        auto a = port_position(w.from), b = port_position(w.to);
        QPainterPath path(a);
        if (w.bends.empty()) {
            auto sa = stub(w.from, a), sb = stub(w.to, b);
            std::vector<double> xs{(sa.x() + sb.x()) / 2}, ys{(sa.y() + sb.y()) / 2};
            for (const auto &box : obstacles) {
                xs.push_back(box.left() - 22);
                xs.push_back(box.right() + 22);
                ys.push_back(box.top() - 32);
                ys.push_back(box.bottom() + 32);
            }
            std::vector<QPointF> best;
            double score = std::numeric_limits<double>::infinity();
            auto consider = [&](std::vector<QPointF> route) {
                if (!clear(route))
                    return;
                auto first = route[1] - route[0], next = route[2] - route[1];
                auto last = route.back() - route[route.size() - 2],
                     previous = route[route.size() - 2] - route[route.size() - 3];
                if (QPointF::dotProduct(first, next) < 0 || QPointF::dotProduct(last, previous) < 0)
                    return;
                double length = 0;
                for (size_t i = 1; i < route.size(); ++i)
                    length +=
                        std::abs(route[i].x() - route[i - 1].x()) + std::abs(route[i].y() - route[i - 1].y());
                if (length < score) {
                    score = length;
                    best = std::move(route);
                }
            };
            for (double x : xs)
                consider({a, sa, {x, sa.y()}, {x, sb.y()}, sb, b});
            for (double y : ys)
                consider({a, sa, {sa.x(), y}, {sb.x(), y}, sb, b});
            if (best.empty())
                best = {a, sa, {sa.x(), sb.y()}, sb, b};
            for (auto point : best)
                path.lineTo(point);
        } else {
            for (auto point : w.bends) {
                path.lineTo(point.x, path.currentPosition().y());
                path.lineTo(point.x, point.y);
            }
            path.lineTo(b.x(), path.currentPosition().y());
            path.lineTo(b);
        }
        found->second->setPath(path);
        auto net = graph.nets.find(endpoint_key(w.from));
        bool active =
            w.id == selected_ || (!highlight.empty() && net != graph.nets.end() && net->second == highlight);
        auto domain = port_type(project(), w.from).domain;
        bool gate = domain == Domain::gate;
        bool signal = domain == Domain::signal;
        found->second->setPen(
            QPen(active ? QColor("#e88b22") : QColor(gate ? "#17866d" : (signal ? "#8c67c8" : "#146cca")),
                 active ? 3 : 2, gate ? Qt::DashLine : Qt::SolidLine));
    }
}
void EditorWindow::fill_inspector() {
    const bool valid = atoms_.count(selected_) || wires_.count(selected_);
    inspector_hint_->setVisible(!valid);
    apply_button_->setVisible(valid);
    auto visible = [&](QWidget *w, bool show) {
        w->setVisible(show);
        if (auto *label = properties_->labelForField(w))
            label->setVisible(show);
    };
    for (QWidget *w :
         std::initializer_list<QWidget *>{name_, value_, initial_, closed_, events_, bends_, inputs_,frequency_,duty_,delay_})
        visible(w, false);
    for (const auto &c : project().components)
        if (c.id == selected_) {
            visible(name_, true);
            name_->setText(q(c.name));
            auto unit = component_unit(c.kind);
            visible(value_, !unit.empty());
            value_->setText(engineering(c.value, unit));
            bool state = c.kind == Kind::capacitor || c.kind == Kind::inductor;
            visible(initial_, state);
            initial_->setText(QString::number(c.initial, 'g', 12) +
                              (c.kind == Kind::capacitor ? " V" : " A"));
            visible(closed_, c.kind == Kind::ideal_switch);
            closed_->setChecked(c.closed);
            visible(events_, c.kind == Kind::ideal_switch);
        }
    for (const auto &n : project().nodes)
        if (n.id == selected_) {
            visible(name_, true);
            name_->setText(q(n.name));
        }
    for (const auto &g : project().patterns)
        if (g.id == selected_) {
            visible(name_, true);
            name_->setText(q(g.name));
            visible(closed_, !g.pwm);
            closed_->setChecked(g.initial);
            visible(events_, !g.pwm);
            visible(frequency_,g.pwm);visible(duty_,g.pwm);visible(delay_,g.pwm);
            frequency_->setText(QString::number(g.frequency,'g',12));duty_->setText(QString::number(g.duty*100,'g',12));delay_->setText(QString::number(g.delay,'g',12));
        }
    for (const auto &plot : project().plots)
        if (plot.id == selected_) {
            visible(name_, true);
            name_->setText(q(plot.name));
            visible(inputs_, true);
            inputs_->setValue(static_cast<int>(plot.inputs));
        }
    QString events;
    for (const auto &e : project().events)
        if (e.target == selected_)
            events += QString::number(e.time, 'g', 12) + "s " + (e.closed ? "1" : "0") + "\n";
    events_->setPlainText(events);
    for (const auto &w : project().wires)
        if (w.id == selected_) {
            visible(bends_, true);
            QString s;
            for (auto b : w.bends)
                s += QString::number(b.x) + " " + QString::number(b.y) + "\n";
            bends_->setPlainText(s);
        }
}
void EditorWindow::apply_inspector() {
    if (running() || selected_.empty())
        return;
    try {
        document_->apply("Edit properties", [&](Project &p) {
            bool gate = false;
            for (auto &c : p.components)
                if (c.id == selected_) {
                    c.name = name_->text().toStdString();
                    auto unit = component_unit(c.kind);
                    if (!unit.empty())
                        c.value = parse_si(value_->text().toStdString(), unit);
                    if (c.kind == Kind::capacitor || c.kind == Kind::inductor)
                        c.initial =
                            parse_si(initial_->text().toStdString(), c.kind == Kind::capacitor ? "V" : "A");
                    if (c.kind == Kind::ideal_switch) {
                        c.closed = closed_->isChecked();
                        gate = true;
                    }
                }
            for (auto &n : p.nodes)
                if (n.id == selected_)
                    n.name = name_->text().toStdString();
            for (auto &g : p.patterns)
                if (g.id == selected_) {
                    g.name = name_->text().toStdString();
                    g.initial = closed_->isChecked();
                    gate = !g.pwm;
                    if(g.pwm){g.frequency=parse_si(frequency_->text().toStdString(),"Hz");g.duty=parse_si(duty_->text().toStdString(),"")/100;g.delay=parse_si(delay_->text().toStdString(),"s");}
                }
            for (auto &plot : p.plots)
                if (plot.id == selected_) {
                    plot.name = name_->text().toStdString();
                    plot.inputs = static_cast<unsigned>(inputs_->value());
                    std::erase_if(p.wires, [&](const Wire &w) {
                        const Endpoint *e =
                            w.from.object == plot.id ? &w.from : (w.to.object == plot.id ? &w.to : nullptr);
                        if (!e)
                            return false;
                        for (unsigned i = 1; i <= plot.inputs; ++i)
                            if (e->port == "in" + std::to_string(i))
                                return false;
                        return true;
                    });
                }
            if (gate) {
                std::erase_if(p.events, [&](const GateEvent &e) { return e.target == selected_; });
                for (const auto &line : events_->toPlainText().split('\n')) {
                    if (line.trimmed().isEmpty())
                        continue;
                    std::istringstream in(line.toStdString());
                    std::string time, extra;
                    int state = -1;
                    if (!(in >> time >> state) || (in >> extra) || (state != 0 && state != 1))
                        throw std::runtime_error(text("events_format").toStdString());
                    p.events.push_back({parse_si(time, "s"), selected_, state != 0});
                }
            }
            for (auto &w : p.wires)
                if (w.id == selected_) {
                    w.bends.clear();
                    for (const auto &line : bends_->toPlainText().split('\n')) {
                        if (line.trimmed().isEmpty())
                            continue;
                        std::istringstream in(line.toStdString());
                        Point point;
                        std::string extra;
                        if (!(in >> point.x >> point.y) || (in >> extra) || !std::isfinite(point.x) ||
                            !std::isfinite(point.y))
                            throw std::runtime_error(text("bends_format").toStdString());
                        w.bends.push_back(point);
                    }
                }
        });
        refresh();
    } catch (const std::exception &e) {
        show_error(e);
    }
}
std::string EditorWindow::add_component(Kind kind, QPointF point) {
    if (running())
        return {};
    try {
        point = snapped(point);
        selected_ = document_->add_component(kind, point.x(), point.y());
        refresh();
        return selected_;
    } catch (const std::exception &e) {
        show_error(e);
        return {};
    }
}
std::string EditorWindow::add_node(bool ground, QPointF point) {
    if (running())
        return {};
    try {
        point = snapped(point);
        selected_ = document_->add_node(ground, point.x(), point.y());
        refresh();
        return selected_;
    } catch (const std::exception &e) {
        show_error(e);
        return {};
    }
}
std::string EditorWindow::add_pattern(QPointF point) {
    if (running())
        return {};
    try {
        point = snapped(point);
        selected_ = document_->add_pattern(point.x(), point.y());
        refresh();
        return selected_;
    } catch (const std::exception &e) {
        show_error(e);
        return {};
    }
}
std::string EditorWindow::add_plot(QPointF point) {
    if (running())
        return {};
    point = snapped(point);
    selected_ = document_->add_plot(point.x(), point.y(), text("plot").toStdString());
    refresh();
    return selected_;
}
bool EditorWindow::connect_ports(Endpoint from, Endpoint to) {
    if (running())
        return false;
    try {
        document_->connect(std::move(from), std::move(to));
        refresh();
        return true;
    } catch (const std::exception &e) {
        show_error(e);
        return false;
    }
}
void EditorWindow::handle_port(Endpoint e) {
    if (running())
        return;
    placing_ = -1;
    if (!pending_port_) {
        pending_port_ = e;
        banner_->setText(text("wire_hint"));
        update_wires();
    } else {
        auto from = *pending_port_;
        pending_port_.reset();
        if (connect_ports(from, e))
            banner_->setText(text("hint"));
        update_wires();
    }
}
void EditorWindow::commit_positions() {
    if (rebuilding_ || running())
        return;
    bool moved = false;
    auto check = [&](const auto &a) {
        if (atoms_.at(a.id)->pos() != QPointF(a.x, a.y))
            moved = true;
    };
    for (const auto &c : project().components)
        check(c);
    for (const auto &n : project().nodes)
        check(n);
    for (const auto &g : project().patterns)
        check(g);
    for (const auto &g : project().plots)
        check(g);
    if (!moved)
        return;
    document_->apply("Move objects", [&](Project &p) {
        auto move = [&](auto &a) {
            auto pos = atoms_.at(a.id)->pos();
            a.x = pos.x();
            a.y = pos.y();
        };
        for (auto &c : p.components)
            move(c);
        for (auto &n : p.nodes)
            move(n);
        for (auto &g : p.patterns)
            move(g);
        for (auto &g : p.plots)
            move(g);
    });
    refresh();
}
void EditorWindow::delete_selected() {
    if (running())
        return;
    std::vector<std::string> ids;
    for (auto *item : canvas_->scene()->selectedItems())
        ids.push_back(item->data(0).toString().toStdString());
    if (ids.empty())
        return;
    document_->erase(ids);
    selected_.clear();
    pending_port_.reset();
    refresh();
}
void EditorWindow::undo() {
    if (!running()) {
        pending_port_.reset();
        document_->undo();
        refresh();
    }
}
void EditorWindow::redo() {
    if (!running()) {
        pending_port_.reset();
        document_->redo();
        refresh();
    }
}
void EditorWindow::select_object(const std::string &id) {
    std::string target = id;
    if (!atoms_.count(target) && !wires_.count(target)) {
        auto graph = resolve_connections(project());
        for (const auto &[endpoint, net] : graph.nets)
            if (net == id) {
                target = endpoint.substr(0, endpoint.find('/'));
                break;
            }
    }
    rebuilding_ = true;
    canvas_->scene()->clearSelection();
    QGraphicsItem *item =
        atoms_.count(target) ? atoms_.at(target) : (wires_.count(target) ? wires_.at(target) : nullptr);
    if (item) {
        item->setSelected(true);
        canvas_->ensureVisible(item);
    }
    selected_ = target;
    rebuilding_ = false;
    fill_inspector();
    update_wires();
}
void EditorWindow::show_error(const std::exception &e) {
    QString message = QString::fromUtf8(e.what());
    auto *d = dynamic_cast<const Diagnostic *>(&e);
    if (d)
        message = q(d->code) + ": " + message;
    auto *item = new QListWidgetItem(message, errors_);
    if (d)
        item->setData(Qt::UserRole, q(d->object));
    bottom_->setCurrentIndex(0);
    banner_->setText(text("error_hint"));
}
void EditorWindow::start_simulation() {
    if (running())
        return;
    try {
        Profile profile = project().profile;
        profile.stop = parse_si(stop_->text().toStdString(), "s");
        profile.step = parse_si(step_->text().toStdString(), "s");
        profile.method = method_->currentIndex() == 1 ? Method::trapezoidal : Method::backward_euler;
        if (profile.stop <= 0 || profile.step <= 0)
            throw std::runtime_error(text("positive_time").toStdString());
        document_->apply("Simulation profile", [&](Project &p) { p.profile = profile; });
        stop_->setText(QString::number(profile.stop,'g',12));step_->setText(QString::number(profile.step,'g',12));
        errors_->clear();
        clear_result();
        scope_enable_->setEnabled(false);
        if (channels_)
            channels_->setEnabled(false);
        cancel_ = false;
        simulated_time_ = 0;
        pending_port_.reset();
        canvas_->setEnabled(false);
        library_->setEnabled(false);
        run_->setEnabled(false);
        undo_->setEnabled(false);
        redo_->setEnabled(false);
        banner_->setText(text("running"));
        busy_ = true;
        const auto snapshot = project();
        const auto keys = recording_keys();
        watcher_.setFuture(QtConcurrent::run([this, snapshot, keys] {
            Outcome outcome;
            try {
                auto ir = compile(snapshot);
                auto catalog = available_channels(ir);
                Recording plan;
                plan.all = false;
                for (const auto &key : keys)
                    if (std::any_of(catalog.begin(), catalog.end(),
                                    [&](const Channel &c) { return c.object == key; }))
                        plan.channels.push_back(key);
                outcome.result = execute(ir, &cancel_, &simulated_time_, &plan);
            } catch (const Diagnostic &e) {
                outcome.error = q(e.code) + ": " + QString::fromUtf8(e.what());
                outcome.object = e.object;
            } catch (const std::exception &e) {
                outcome.error = QString::fromUtf8(e.what());
            }
            return outcome;
        }));
        update_title();
    } catch (const std::exception &e) {
        show_error(e);
    }
}
void EditorWindow::stop_simulation() {
    canvas_->cancel_wire();canvas_->set_ghost(nullptr);
    if (running()) {
        cancel_ = true;
        banner_->setText(text("stopping"));
    } else {
        pending_port_.reset();
        placing_ = -1;
        library_->clearSelection();
        banner_->setText(text("hint"));
        update_wires();
    }
}
void EditorWindow::finish_simulation() {
    auto completed = watcher_.future();
    auto outcome = completed.takeResult();
    busy_ = false;
    canvas_->setEnabled(true);
    library_->setEnabled(true);
    run_->setEnabled(true);
    scope_enable_->setEnabled(true);
    if (channels_)
        channels_->setEnabled(true);
    undo_->setEnabled(document_->can_undo());
    redo_->setEnabled(document_->can_redo());
    if (!outcome.error.isEmpty()) {
        auto *item = new QListWidgetItem(outcome.error, errors_);
        item->setData(Qt::UserRole, q(outcome.object));
        bottom_->setCurrentIndex(0);
        banner_->setText(text("error_hint"));
        return;
    }
    result_ = std::move(outcome.result);
    findChild<QLabel *>("diagnostic_status")
        ->setText(text(result_->cancelled ? "cancelled" : "diagnostics_ok") + " · " +
                  QString::number(result_->accepted_steps) + " " + text("steps"));
    choose_channels();
    update_graphs();
    banner_->setText(text(result_->cancelled ? "cancelled" : "complete") + " · " +
                     QString::number(result_->accepted_steps) + " " + text("steps") +
                     (result_->samples.empty() ? " · " + text("no_recording") : QString()));
    update_title();
}
void EditorWindow::choose_channels() {
    if (!channels_)
        return;
    std::vector<std::string> keys;
    const QStringList colors{"#146cca", "#c56819", "#17866d", "#935ad5", "#d04769"};
    bool prior = rebuilding_;
    rebuilding_ = true;
    for (int i = 0; i < channels_->count(); ++i) {
        auto *item = channels_->item(i);
        if (item->checkState() == Qt::Checked) {
            item->setForeground(QColor(colors[static_cast<int>(keys.size()) % colors.size()]));
            keys.push_back(item->data(Qt::UserRole).toString().toStdString());
        } else
            item->setForeground(QColor("#8390a3"));
    }
    rebuilding_ = prior;
    if (keys != project().scope_channels)
        document_->apply("Scope channels", [&](Project &p) { p.scope_channels = keys; });
    if (scope_)
        scope_->set_result(result_ ? &*result_ : nullptr, result_indices(keys), project());
    scope_export_->setEnabled(result_ && !result_->samples.empty() && !result_indices(keys).empty());
    update_title();
}
void EditorWindow::update_title() {
    if (!document_)
        return;
    setWindowTitle("PowerDriveSim — " + (path_.isEmpty() ? q(project().name) : QFileInfo(path_).fileName()) +
                   (serialized(project()) == saved_state_ ? "" : " *"));
    statusBar()->showMessage(QString::number(project().components.size()) + " " + text("components") + " · " +
                             QString::number(project().wires.size()) + " " + text("wires"));
}
bool EditorWindow::confirm_discard() {
    if (!document_ || serialized(project()) == saved_state_)
        return true;
    auto choice = QMessageBox::question(this, text("unsaved"), text("save_question"),
                                        QMessageBox::Save | QMessageBox::Discard | QMessageBox::Cancel);
    if (choice == QMessageBox::Cancel)
        return false;
    if (choice == QMessageBox::Discard)
        return true;
    auto path = path_;
    if (path.isEmpty())
        path = QFileDialog::getSaveFileName(this, text("save"), {}, text("project_filter"));
    return !path.isEmpty() && save_project(path);
}
void EditorWindow::closeEvent(QCloseEvent *e) {
    if (running()) {
        stop_simulation();
        e->ignore();
        return;
    }
    if (confirm_discard())
        e->accept();
    else
        e->ignore();
}
void EditorWindow::export_csv(const std::vector<std::string> &keys) {
    if (!result_)
        return;
    auto path = QFileDialog::getSaveFileName(this, text("export"), {}, "CSV (*.csv)");
    if (path.isEmpty())
        return;
    try {
        std::ostringstream stream;
        write_csv(select_result(*result_, keys), stream);
        auto bytes = stream.str();
        QSaveFile file(path);
        if (!file.open(QIODevice::WriteOnly) ||
            file.write(bytes.data(), static_cast<qint64>(bytes.size())) !=
                static_cast<qint64>(bytes.size()) ||
            !file.commit())
            throw std::runtime_error(file.errorString().toStdString());
        banner_->setText(text("saved"));
    } catch (const std::exception &e) {
        show_error(e);
    }
}
} // namespace pds::desktop
