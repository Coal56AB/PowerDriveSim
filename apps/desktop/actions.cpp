#include "apps/desktop/editor.hpp"
#include "formats/project/project.hpp"
#include <QAction>
#include <QApplication>
#include <QClipboard>
#include <QDialog>
#include <QDialogButtonBox>
#include <QDir>
#include <QGraphicsPathItem>
#include <QGraphicsScene>
#include <QHeaderView>
#include <QKeySequenceEdit>
#include <QLabel>
#include <QLineEdit>
#include <QMenu>
#include <QMimeData>
#include <QPainterPath>
#include <QPushButton>
#include <QSettings>
#include <QTableWidget>
#include <QVBoxLayout>
#include <algorithm>
#include <cmath>
#include <limits>
#include <sstream>
namespace pds::desktop {
std::vector<std::string> EditorWindow::selected_ids() const {
    std::vector<std::string> ids;
    for(auto* item:canvas_->scene()->selectedItems()){auto id=item->data(0).toString().toStdString();if(!id.empty())ids.push_back(id);}
    return ids;
}
void EditorWindow::transform_selection(int turns,bool mirror){
    if(running())return;auto ids=selected_ids();if(ids.empty())return;
    try{document_->transform(ids,turns,mirror);refresh();for(const auto& id:ids)if(atoms_.count(id))atoms_.at(id)->setSelected(true);}catch(const std::exception& e){show_error(e);}
}
void EditorWindow::arrange_selection(const std::string& mode){
    if(running())return;auto ids=selected_ids();if(ids.size()<2)return;
    try{document_->arrange(ids,mode);refresh();for(const auto& id:ids)if(atoms_.count(id))atoms_.at(id)->setSelected(true);}catch(const std::exception& e){show_error(e);}
}
bool EditorWindow::copy_selection(bool cut){
    if(cut&&running())return false;auto ids=selected_ids();if(ids.empty())return false;
    auto fragment=document_->copy(ids);if(fragment.components.empty()&&fragment.nodes.empty()&&fragment.patterns.empty()&&fragment.plots.empty())return false;
    try{std::ostringstream out;write_project(fragment,out);auto* mime=new QMimeData;auto bytes=QByteArray::fromStdString(out.str());mime->setData("application/x-powerdrivesim-project",bytes);mime->setText(QString::fromUtf8(bytes));QApplication::clipboard()->setMimeData(mime);paste_count_=0;if(cut){document_->erase(ids);selected_.clear();refresh();}return true;}catch(const std::exception& e){show_error(e);return false;}
}
void EditorWindow::paste_selection(bool duplicate){
    if(running())return;
    try{Project fragment;
        if(duplicate)fragment=document_->copy(selected_ids());
        else{const auto* mime=QApplication::clipboard()->mimeData();if(!mime->hasFormat("application/x-powerdrivesim-project"))return;std::istringstream in(mime->data("application/x-powerdrivesim-project").toStdString());fragment=read_project(in);}
        if(fragment.components.empty()&&fragment.nodes.empty()&&fragment.patterns.empty()&&fragment.plots.empty())return;
        double offset=duplicate?40:40*++paste_count_;auto ids=document_->paste(fragment,offset,offset);selected_.clear();refresh();for(const auto& id:ids)if(atoms_.count(id))atoms_.at(id)->setSelected(true);
    }catch(const std::exception& e){show_error(e);}
}
void EditorWindow::show_context(const std::string& id,QPoint global){
    if(!id.empty()){auto* item=atoms_.count(id)?atoms_.at(id):(wires_.count(id)?wires_.at(id):nullptr);if(item&&!item->isSelected())select_object(id);}
    QMenu menu(this);menu.setObjectName("element_context");
    if(!id.empty()){
        menu.addAction(commands_.at("properties"));
        if(std::any_of(project().plots.begin(),project().plots.end(),[&](const PlotBlock& g){return g.id==id;}))menu.addAction(text("plot"),this,[this,id]{open_plot(id);});
        auto* arrange=menu.addMenu(text("arrange"));for(const char* key:{"rotate","rotate_back","mirror","left","right","top","bottom","horizontal","vertical"})arrange->addAction(commands_.at(key));
        menu.addSeparator();for(const char* key:{"copy","cut","duplicate","delete"})menu.addAction(commands_.at(key));
        menu.addAction(text("observe"),this,[this,id]{observe_object(id);});
    }
    menu.addAction(commands_.at("paste"));menu.addSeparator();menu.addAction(commands_.at("shortcuts"));menu.exec(global);
}
void EditorWindow::load_shortcuts(){
    QSettings settings(recovery_dir_+"/shortcuts.ini",QSettings::IniFormat);
    for(auto& [id,action]:commands_){auto key=QString::fromStdString(id);if(settings.contains(key))action->setShortcut(QKeySequence::fromString(settings.value(key).toString(),QKeySequence::PortableText));}
}
void EditorWindow::show_shortcuts(){
    QDialog dialog(this);dialog.setObjectName("shortcuts_dialog");dialog.setWindowTitle(text("shortcuts"));dialog.resize(670,640);
    auto* layout=new QVBoxLayout(&dialog);layout->setContentsMargins(20,20,20,20);layout->setSpacing(14);
    auto* title=new QLabel(text("shortcuts"));title->setStyleSheet("font-size:18px;font-weight:600;color:#253e5c");layout->addWidget(title);
    auto* table=new QTableWidget(static_cast<int>(commands_.size()),2);table->setHorizontalHeaderLabels({text("action_label"),text("shortcut_label")});table->verticalHeader()->hide();table->horizontalHeader()->setSectionResizeMode(QHeaderView::Stretch);table->setSelectionMode(QAbstractItemView::NoSelection);table->setShowGrid(false);layout->addWidget(table);
    std::map<std::string,QKeySequenceEdit*> editors;int row=0;
    for(auto& [id,action]:commands_){auto* name=new QTableWidgetItem(action->text());name->setFlags(Qt::ItemIsEnabled);table->setItem(row,0,name);auto* edit=new QKeySequenceEdit(action->shortcut());edit->setObjectName("shortcut_"+QString::fromStdString(id));edit->setMaximumSequenceLength(1);table->setCellWidget(row,1,edit);table->setRowHeight(row++,35);editors[id]=edit;}
    auto* error=new QLabel;error->setObjectName("shortcut_error");error->setWordWrap(true);error->setStyleSheet("color:#b8394e");layout->addWidget(error);
    auto* buttons=new QDialogButtonBox(QDialogButtonBox::Save|QDialogButtonBox::Cancel|QDialogButtonBox::RestoreDefaults);layout->addWidget(buttons);
    connect(buttons,&QDialogButtonBox::rejected,&dialog,&QDialog::reject);
    connect(buttons->button(QDialogButtonBox::RestoreDefaults),&QPushButton::clicked,&dialog,[&]{for(auto& [id,edit]:editors)edit->setKeySequence(default_shortcuts_.at(id));error->clear();});
    connect(buttons,&QDialogButtonBox::accepted,&dialog,[&]{
        for(auto a=editors.begin();a!=editors.end();++a)for(auto b=std::next(a);b!=editors.end();++b){auto x=a->second->keySequence(),y=b->second->keySequence();if(!x.isEmpty()&&!y.isEmpty()&&(x.matches(y)!=QKeySequence::NoMatch||y.matches(x)!=QKeySequence::NoMatch)){error->setText(text("shortcut_conflict")+" "+commands_.at(a->first)->text()+" / "+commands_.at(b->first)->text());return;}}
        QDir().mkpath(recovery_dir_);QSettings settings(recovery_dir_+"/shortcuts.ini",QSettings::IniFormat);for(auto& [id,edit]:editors){commands_.at(id)->setShortcut(edit->keySequence());settings.setValue(QString::fromStdString(id),edit->keySequence().toString(QKeySequence::PortableText));}settings.sync();dialog.accept();
    });dialog.exec();
}
void EditorWindow::finish_wire(Endpoint from,std::optional<Endpoint> to,QPointF point,const std::string& target){
    if(running())return;pending_port_.reset();
    if(to){connect_ports(from,*to);return;}
    try{
        if(!target.empty()){
            auto found=std::find_if(project().wires.begin(),project().wires.end(),[&](const Wire& w){return w.id==target;});if(found==project().wires.end())return;Wire old=*found;
            bool physical=port_type(project(),old.from).domain==Domain::electrical&&port_type(project(),old.to).domain==Domain::electrical;
            if(!physical){auto source=port_type(project(),old.from).direction==Direction::output||port_type(project(),old.from).domain==Domain::electrical?old.from:old.to;if(source!=from)connect_ports(from,source);return;}
            // Project the drop onto the displayed polyline, preserving both halves exactly.
            auto path=wires_.at(target)->path();double best=std::numeric_limits<double>::infinity();int segment=1;QPointF joint;
            for(int i=1;i<path.elementCount();++i){auto ea=path.elementAt(i-1),eb=path.elementAt(i);QPointF a(ea.x,ea.y),b(eb.x,eb.y),d=b-a;double length=QPointF::dotProduct(d,d);double t=length?std::clamp(QPointF::dotProduct(point-a,d)/length,0.,1.):0;auto candidate=a+t*d;double distance=QPointF::dotProduct(point-candidate,point-candidate);if(distance<best){best=distance;joint=candidate;segment=i;}}
            if(QLineF(joint,port_position(old.from)).length()<1){if(from!=old.from)connect_ports(from,old.from);return;}
            if(QLineF(joint,port_position(old.to)).length()<1){if(from!=old.to)connect_ports(from,old.to);return;}
            auto id=new_uuid();Endpoint node{id,"node"};
            document_->apply("Branch wire",[&](Project& p){
                p.nodes.push_back({id,"",false,joint.x(),joint.y()});
                auto it=std::find_if(p.wires.begin(),p.wires.end(),[&](const Wire& w){return w.id==target;});it->to=node;it->bends.clear();
                for(int i=1;i<segment;++i){auto e=path.elementAt(i);it->bends.push_back({e.x,e.y});}
                Wire second{new_uuid(),node,old.to,{}};for(int i=segment;i<path.elementCount()-1;++i){auto e=path.elementAt(i);second.bends.push_back({e.x,e.y});}p.wires.push_back(second);p.wires.push_back({new_uuid(),from,node,{}});
            });selected_=id;refresh();return;
        }
        if(port_type(project(),from).domain!=Domain::electrical)return;
        point={std::round(point.x()/20)*20,std::round(point.y()/20)*20};auto id=new_uuid();
        document_->apply("Extend wire",[&](Project& p){p.nodes.push_back({id,"",false,point.x(),point.y()});p.wires.push_back({new_uuid(),from,{id,"node"},{}});});selected_=id;refresh();
    }catch(const std::exception& e){show_error(e);}
}
}
