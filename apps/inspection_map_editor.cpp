#include "damage_store.h"
#include "inspection_map_engine.h"
#include "inspection_map_workflow.h"

#include <QApplication>
#include <QComboBox>
#include <QFileDialog>
#include <QFile>
#include <QFileInfo>
#include <QGraphicsPathItem>
#include <QGraphicsPolygonItem>
#include <QGraphicsScene>
#include <QGraphicsView>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QLabel>
#include <QListWidget>
#include <QMainWindow>
#include <QMessageBox>
#include <QPageSize>
#include <QPainter>
#include <QPdfWriter>
#include <QBuffer>
#include <QImage>
#include <QImageReader>
#include <QImageIOHandler>
#include <QTransform>
#include <QSaveFile>
#include <QPushButton>
#include <QResizeEvent>
#include <QSplitter>
#include <QStandardItemModel>
#include <QSqlDatabase>
#include <QSqlError>
#include <QSqlQuery>
#include <QStatusBar>
#include <QTableWidget>
#include <QTimer>
#include <QVBoxLayout>
#include <QWheelEvent>
#include <QMouseEvent>
#include <functional>
#include <algorithm>
#include <array>

namespace {
class DrawingView : public QGraphicsView {
public:
    bool capturePoints = false;
    std::function<void(const QPointF &)> pointPicked;
    explicit DrawingView(QWidget *parent = nullptr) : QGraphicsView(parent) {
        setRenderHint(QPainter::Antialiasing);
        setDragMode(QGraphicsView::ScrollHandDrag);
        setBackgroundBrush(QColor(248, 250, 250));
        setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
        setVerticalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    }
    void fit() {
        if (scene() && !scene()->items().isEmpty())
            fitInView(scene()->itemsBoundingRect().adjusted(-0.5, -0.5, 0.5, 0.5), Qt::KeepAspectRatio);
    }
protected:
    void mousePressEvent(QMouseEvent *event) override {
        if (capturePoints && pointPicked && event->button() == Qt::LeftButton) {
            pointPicked(mapToScene(event->pos())); event->accept(); return;
        }
        QGraphicsView::mousePressEvent(event);
    }
    void wheelEvent(QWheelEvent *event) override {
        const double factor = event->angleDelta().y() > 0 ? 1.15 : 1.0 / 1.15;
        setTransformationAnchor(QGraphicsView::AnchorUnderMouse);
        scale(factor, factor);
        event->accept();
    }
};

QString statusLabel(const QString &status) {
    if (status == QStringLiteral("PROJECTED")) return QStringLiteral("투영 완료");
    if (status == QStringLiteral("EXCLUDED")) return QStringLiteral("검토 제외");
    if (status == QStringLiteral("NEEDS_REVIEW")) return QStringLiteral("확인 필요");
    return QStringLiteral("투영 불가");
}

QString dimensionLabel(double value) {
    return value > 0 ? QString::number(value, 'f', 1) : QStringLiteral("—");
}

QString metreLabel(double millimetres) {
    if (millimetres <= 0) return QStringLiteral("—");
    QString value = QString::number(millimetres / 1000.0, 'f', 3);
    while (value.endsWith(QLatin1Char('0')) && !value.endsWith(QStringLiteral(".0"))) value.chop(1);
    return value;
}

QString typeLabel(const QString &type) {
    if (type == QStringLiteral("crack")) return QStringLiteral("균열");
    if (type == QStringLiteral("crack_region")) return QStringLiteral("균열");
    if (type == QStringLiteral("spalling")) return QStringLiteral("박리");
    if (type == QStringLiteral("exposed_rebar")) return QStringLiteral("철근 노출");
    if (type == QStringLiteral("leakage_wetspot")) return QStringLiteral("누수·습윤");
    return type;
}

QString quantityTypeLabel(const ProjectedDamage &damage) {
    if (damage.type != QStringLiteral("crack") && damage.type != QStringLiteral("crack_region"))
        return typeLabel(damage.type);
    if (damage.crackP90Mm <= 0) return QStringLiteral("균열");
    return damage.crackP90Mm >= 0.3 ? QStringLiteral("균열 (cw≥0.3mm)") :
        QStringLiteral("균열 (cw<0.3mm)");
}

QTransform previewOrientation(QImageIOHandler::Transformations flags, const QSize &size) {
    const double width = size.width(), height = size.height();
    switch (int(flags)) {
    case 1: return QTransform(-1, 0, 0, 1, width, 0);
    case 2: return QTransform(1, 0, 0, -1, 0, height);
    case 3: return QTransform(-1, 0, 0, -1, width, height);
    case 4: return QTransform(0, 1, -1, 0, height, 0);
    case 5: return QTransform(0, 1, 1, 0, 0, 0);
    case 6: return QTransform(0, -1, -1, 0, height, width);
    case 7: return QTransform(0, -1, 1, 0, 0, width);
    default: return {};
    }
}

QVector<const ProjectedDamage *> numberedDamages(const ProjectionBatch &batch, const QString &part,
                                                  const UnrollSheet *sheet = nullptr) {
    QVector<const ProjectedDamage *> result;
    for (const auto &item : batch.items) {
        if (item.status != QStringLiteral("PROJECTED") ||
            !(sheet ? sheet->parts.contains(item.partId) : item.partId == part) ||
            (sheet && sheet->side != QStringLiteral("GEN") &&
             item.faceId.section(QLatin1Char('-'), 4, 4) != sheet->side)) continue;
        if (sheet) {
            bool mapped = !item.paths.isEmpty();
            for (const auto &path : item.paths) for (const auto &point : path) {
                QPointF destination;
                if (!sheet->point(item.faceId, point, &destination)) mapped = false;
            }
            if (!mapped) continue;
        }
        result.append(&item);
    }
    std::sort(result.begin(), result.end(), [](const auto *a, const auto *b) {
        if (a->partId != b->partId) return a->partId < b->partId;
        if (a->imageName != b->imageName) return a->imageName < b->imageName;
        return a->objectId < b->objectId;
    });
    return result;
}

int damageNumber(const QVector<const ProjectedDamage *> &numbered, const ProjectedDamage &item) {
    for (int i = 0; i < numbered.size(); ++i) if (numbered[i] == &item) return i + 1;
    return 0;
}

void addNumber(QGraphicsScene *scene, const QPointF &anchor, int number) {
    if (number <= 0) return;
    QPointF label = anchor + QPointF(0.34, -0.34);
    for (int attempt = 0; attempt < 12; ++attempt) {
        bool overlap = false;
        for (const auto *item : scene->items()) {
            if (!item->data(0).toBool()) continue;
            if (QLineF(item->sceneBoundingRect().center(), label).length() < 0.48) {
                overlap = true; break;
            }
        }
        if (!overlap) break;
        label = anchor + QPointF(0.34 + 0.5 * (attempt / 3 + 1), -0.34 - 0.5 * (attempt % 3));
    }
    scene->addLine(QLineF(anchor, label), QPen(QColor(0, 125, 55), 0))->setZValue(3);
    auto *circle = scene->addEllipse(QRectF(label.x()-0.22, label.y()-0.22, 0.44, 0.44),
        QPen(QColor(0, 125, 55), 0), QBrush(Qt::white));
    circle->setZValue(4);
    circle->setData(0, true);
    auto *text = scene->addText(QString::number(number));
    text->setDefaultTextColor(QColor(0, 125, 55));
    text->setScale(0.019);
    text->setPos(label.x()-0.16, label.y()-0.18);
    text->setZValue(5);
}

void fillScene(QGraphicsScene *scene, const MapDrawing &drawing, const ProjectionBatch &batch,
               const QString &selectedObject = {}) {
    scene->clear();
    const auto numbered = numberedDamages(batch, drawing.partId);
    const QPen outline(QColor(86, 106, 116), 0);
    for (const auto &line : drawing.lines) {
        QPainterPath path;
        for (int i = 0; i < line.size(); ++i) {
            const auto point = InspectionMapEngine::drawingPoint(drawing, line[i]);
            if (i == 0) path.moveTo(point);
            else path.lineTo(point);
        }
        scene->addPath(path, outline);
    }
    for (const auto &item : batch.items) {
        if (item.drawingId != drawing.id || item.paths.isEmpty() ||
            item.status == QStringLiteral("EXCLUDED")) continue;
        bool numberedHere = false;
        const bool selected = item.objectId == selectedObject;
        const QColor color = item.status == QStringLiteral("PROJECTED") ? QColor(196, 52, 49) : QColor(179, 132, 21);
        QPen pen(selected ? QColor(0, 141, 112) : color, selected ? 3.5 : 2);
        pen.setCosmetic(true);
        if (item.status != QStringLiteral("PROJECTED")) pen.setStyle(Qt::DashLine);
        for (const auto &line : item.paths) {
            QPainterPath path;
            for (int i = 0; i < line.size(); ++i) {
                const auto point = InspectionMapEngine::drawingPoint(drawing, line[i]);
                if (i == 0) path.moveTo(point);
                else path.lineTo(point);
            }
            if (line.size() == 1) path.addEllipse(path.currentPosition(), 0.015, 0.015);
            auto *drawn = scene->addPath(path, pen);
            drawn->setZValue(selected ? 2 : 1);
            drawn->setToolTip(item.imageName + QStringLiteral(" · ") + item.faceId +
                              QStringLiteral(" · ") + statusLabel(item.status));
            if (!numberedHere && !line.isEmpty() && item.status == QStringLiteral("PROJECTED")) {
                addNumber(scene, InspectionMapEngine::drawingPoint(drawing, line.first()), damageNumber(numbered, item));
                numberedHere = true;
            }
        }
    }
    scene->setSceneRect(scene->itemsBoundingRect().adjusted(-0.3, -0.3, 0.3, 0.3));
}

QRectF drawingBounds(const MapDrawing &drawing) {
    QRectF bounds;
    bool initialized = false;
    for (const auto &line : drawing.lines) for (const auto &world : line) {
        const QRectF vertex(InspectionMapEngine::drawingPoint(drawing, world), QSizeF());
        bounds = initialized ? bounds.united(vertex) : vertex;
        initialized = true;
    }
    return bounds;
}

void addDrawingAt(QGraphicsScene *scene, const MapDrawing &drawing, const ProjectionBatch &batch,
                  const QPointF &offset, const QVector<const ProjectedDamage *> &numbered,
                  bool showDamage = true) {
    const QPen outline(QColor(86, 106, 116), 0);
    for (const auto &line : drawing.lines) {
        QPainterPath path;
        for (int i = 0; i < line.size(); ++i) {
            const auto point = InspectionMapEngine::drawingPoint(drawing, line[i]) + offset;
            if (i == 0) path.moveTo(point); else path.lineTo(point);
        }
        scene->addPath(path, outline);
    }
    if (!showDamage) return;
    for (const auto &damage : batch.items) {
        if (damage.drawingId != drawing.id || damage.status != QStringLiteral("PROJECTED")) continue;
        QPen pen(QColor(196, 52, 49), 2); pen.setCosmetic(true);
        bool numberedHere = false;
        for (const auto &line : damage.paths) {
            QPainterPath path;
            for (int i = 0; i < line.size(); ++i) {
                const auto point = InspectionMapEngine::drawingPoint(drawing, line[i]) + offset;
                if (i == 0) path.moveTo(point); else path.lineTo(point);
            }
            if (line.size() == 1) path.addEllipse(path.currentPosition(), 0.015, 0.015);
            scene->addPath(path, pen)->setZValue(2);
            if (!numberedHere && !line.isEmpty()) {
                addNumber(scene, InspectionMapEngine::drawingPoint(drawing, line.first()) + offset,
                          damageNumber(numbered, damage));
                numberedHere = true;
            }
        }
    }
}

QString drawingSide(const MapDrawing &drawing) {
    const QString suffix = drawing.id.mid(drawing.partId.size() + 1);
    if (suffix.startsWith(QStringLiteral("EX-"))) return QStringLiteral("EX");
    if (suffix.startsWith(QStringLiteral("IN-"))) return QStringLiteral("IN");
    if (suffix == QStringLiteral("TP") || suffix == QStringLiteral("BT")) return QStringLiteral("CAP");
    return QStringLiteral("SINGLE");
}

int directionRank(const MapDrawing &drawing) {
    const QString name = drawing.id.mid(drawing.partId.size() + 1).section(QLatin1Char('-'), -1);
    const QStringList order{QStringLiteral("FR"), QStringLiteral("RG"), QStringLiteral("RR"),
                            QStringLiteral("LF"), QStringLiteral("TP"), QStringLiteral("BT")};
    const int rank = order.indexOf(name);
    return rank < 0 ? 100 : rank;
}

void fillMapSheetScene(QGraphicsScene *scene, const QString &part, const InspectionMapEngine &engine,
                       const ProjectionBatch &batch) {
    scene->clear();
    const auto numbered = numberedDamages(batch, part);
    QHash<QString, QVector<const MapDrawing *>> groups;
    for (const auto &drawing : engine.drawings())
        if (drawing.partId == part) groups[drawingSide(drawing)].append(&drawing);
    const bool splitSides = !groups.value(QStringLiteral("EX")).isEmpty() ||
                            !groups.value(QStringLiteral("IN")).isEmpty();
    const QStringList rows = splitSides ? QStringList{QStringLiteral("EX"), QStringLiteral("IN"), QStringLiteral("SINGLE"), QStringLiteral("CAP")} :
                                        QStringList{QStringLiteral("SINGLE"), QStringLiteral("CAP")};
    double y = 0;
    for (const auto &group : rows) {
        auto views = groups.value(group);
        if (views.isEmpty()) continue;
        std::sort(views.begin(), views.end(), [](const auto *a, const auto *b) {
            const int ar = directionRank(*a), br = directionRank(*b);
            return ar == br ? a->id < b->id : ar < br;
        });
        auto *heading = scene->addText(group == QStringLiteral("EX") ? QStringLiteral("외측 EX") :
            group == QStringLiteral("IN") ? QStringLiteral("내측 IN") :
            group == QStringLiteral("CAP") ? QStringLiteral("상면 · 하면") : QStringLiteral("방향도"));
        heading->setDefaultTextColor(QColor(23, 58, 69)); heading->setScale(0.025); heading->setPos(0, y);
        y += 0.55;
        double x = 0, rowHeight = 0;
        for (const auto *drawing : views) {
            const QRectF bounds = drawingBounds(*drawing);
            if (bounds.isEmpty()) continue;
            auto *label = scene->addText(drawing->id.mid(part.size() + 1));
            label->setDefaultTextColor(QColor(43, 70, 80)); label->setScale(0.019);
            label->setPos(x, y);
            addDrawingAt(scene, *drawing, batch, QPointF(x-bounds.left(), y+0.36-bounds.top()), numbered);
            x += bounds.width()+0.6;
            rowHeight = std::max(rowHeight, bounds.height());
        }
        y += rowHeight+1.05;
    }
    if (!scene->items().isEmpty()) scene->setSceneRect(scene->itemsBoundingRect().adjusted(-0.4, -0.4, 0.4, 0.4));
}

QPointF verifiedUnrollOffset(const UnrollSheet &sheet, const InspectionMapEngine &engine) {
    QRectF referenceBounds;
    bool hasReference = false;
    for (const auto &drawing : engine.drawings()) {
        if (!sheet.parts.contains(drawing.partId) ||
            !(sheet.side == QStringLiteral("GEN") ? drawing.id.endsWith(QStringLiteral("-FR")) :
              drawing.id.endsWith(sheet.side + QStringLiteral("-FR")))) continue;
        const QRectF bounds = drawingBounds(drawing);
        referenceBounds = hasReference ? referenceBounds.united(bounds) : bounds;
        hasReference = true;
    }
    if (sheet.faces.isEmpty() || sheet.faces.first().triangles.isEmpty()) return {};
    double minX = 1e9;
    for (const auto &face : sheet.faces) for (const auto &point : face.outline)
        minX = std::min(minX, point.x());
    const auto &sample = sheet.faces.first().triangles.first();
    return QPointF((hasReference ? referenceBounds.right()+1.6 : 0)-minX,
                   -sample.source.a.z()-sample.a.y());
}

void fillVerifiedUnrollScene(QGraphicsScene *scene, const UnrollSheet &sheet,
                             const InspectionMapEngine &engine, const ProjectionBatch &batch) {
    scene->clear();
    const auto numbered = numberedDamages(batch, {}, &sheet);
    QRectF referenceBounds;
    bool hasReference = false;
    for (const auto &drawing : engine.drawings()) {
        if (!sheet.parts.contains(drawing.partId) ||
            !(sheet.side == QStringLiteral("GEN") ? drawing.id.endsWith(QStringLiteral("-FR")) :
              drawing.id.endsWith(sheet.side + QStringLiteral("-FR")))) continue;
        const QRectF bounds = drawingBounds(drawing);
        referenceBounds = hasReference ? referenceBounds.united(bounds) : bounds;
        hasReference = true;
        addDrawingAt(scene, drawing, batch, {}, {}, false);
    }
    if (sheet.faces.isEmpty()) return;
    const QPointF shift = verifiedUnrollOffset(sheet, engine);
    const double xShift = shift.x(), yShift = shift.y();
    for (const auto &face : sheet.faces) {
        QPolygonF polygon;
        for (const auto &point : face.outline) polygon.append(point+QPointF(xShift, yShift));
        scene->addPolygon(polygon, QPen(QColor(92, 113, 122), 0),
            QBrush(sheet.side == QStringLiteral("IN") ? QColor(238, 246, 248) : QColor(252, 253, 253)));
    }
    for (const auto &damage : batch.items) {
        if (damage.status != QStringLiteral("PROJECTED") || !sheet.parts.contains(damage.partId)) continue;
        QPen pen(QColor(197, 48, 45), 2); pen.setCosmetic(true);
        bool numberedHere = false;
        for (const auto &line : damage.paths) {
            QPainterPath path; bool valid = true;
            for (int i = 0; i < line.size(); ++i) {
                QPointF point;
                if (!sheet.point(damage.faceId, line[i], &point)) { valid = false; break; }
                point += QPointF(xShift, yShift);
                if (i == 0) path.moveTo(point); else path.lineTo(point);
            }
            if (valid && !line.isEmpty()) {
                if (line.size() == 1) path.addEllipse(path.currentPosition(), 0.015, 0.015);
                scene->addPath(path, pen)->setZValue(2);
                if (!numberedHere) {
                    QPointF point;
                    if (sheet.point(damage.faceId, line.first(), &point))
                        addNumber(scene, point+QPointF(xShift, yShift), damageNumber(numbered, damage));
                    numberedHere = true;
                }
            }
        }
    }
    for (const auto &part : sheet.parts) {
        double low = 1e9, high = -1e9;
        for (const auto &face : sheet.faces) if (face.partId == part)
            for (const auto &point : face.outline) {
                low = std::min(low, point.y()+yShift); high = std::max(high, point.y()+yShift);
            }
        if (low > high) continue;
        auto *label = scene->addText(part.section(QLatin1Char('-'), -1));
        label->setDefaultTextColor(QColor(151, 50, 43)); label->setScale(0.03);
        label->setPos(xShift-0.8, (low+high)/2-0.25);
    }
    if (hasReference) {
        auto *reference = scene->addText(QStringLiteral("기준 입면"));
        reference->setScale(0.035);
        reference->setPos(referenceBounds.left(), scene->itemsBoundingRect().top()-0.8);
    }
    auto *unrolled = scene->addText(sheet.side == QStringLiteral("EX") ? QStringLiteral("외측 전개") :
        sheet.side == QStringLiteral("IN") ? QStringLiteral("내측 전개") : QStringLiteral("측면 전개"));
    unrolled->setScale(0.035);
    unrolled->setPos(xShift, scene->itemsBoundingRect().top()-0.8);
    scene->setSceneRect(scene->itemsBoundingRect().adjusted(-0.5, -0.5, 0.5, 0.5));
}

void drawQuantityTable(QPainter *painter, QPdfWriter *pdf, const QRect &area,
                       const QVector<const ProjectedDamage *> &damages) {
    const int left = area.left(), right = area.right(), rowHeight = 24;
    const int width = right-left;
    const QVector<int> edges{left, left+width*9/100, left+width*51/100,
        left+width*66/100, left+width*82/100, right};
    int y = area.top();
    auto header = [&] {
        painter->setPen(QColor(30, 53, 68));
        QFont font = painter->font(); font.setBold(true); font.setPixelSize(18); painter->setFont(font);
        painter->drawText(left, y-10, QStringLiteral("손상 물량표"));
        font.setBold(false); font.setPixelSize(13); painter->setFont(font);
        const QStringList names{QStringLiteral("번호"), QStringLiteral("유형"), QStringLiteral("폭"),
            QStringLiteral("길이"), QStringLiteral("단위")};
        for (int c = 0; c < names.size(); ++c)
            painter->drawText(QRect(edges[c]+4, y, edges[c+1]-edges[c]-8, rowHeight),
                              Qt::AlignVCenter | Qt::AlignLeft, names[c]);
        painter->drawLine(left, y+rowHeight, right, y+rowHeight);
        y += rowHeight;
    };
    header();
    for (int i = 0; i < damages.size(); ++i) {
        if (y+rowHeight > area.bottom()) {
            pdf->newPage(); y = area.top(); header();
        }
        const auto &d = *damages[i];
        const QStringList values{QString::number(i+1), quantityTypeLabel(d),
            metreLabel(d.boxWidthMm), metreLabel(d.boxLengthMm), QStringLiteral("m")};
        for (int c = 0; c < values.size(); ++c)
            painter->drawText(QRect(edges[c]+4, y, edges[c+1]-edges[c]-8, rowHeight),
                              Qt::AlignVCenter | Qt::AlignLeft, painter->fontMetrics().elidedText(
                                  values[c], Qt::ElideRight, edges[c+1]-edges[c]-8));
        painter->setPen(QColor(205, 215, 219)); painter->drawLine(left, y+rowHeight, right, y+rowHeight);
        painter->setPen(QColor(30, 53, 68)); y += rowHeight;
    }
    painter->setPen(QColor(82, 107, 120));
    painter->drawText(QRect(left, area.bottom()+8, right-left, 35), Qt::TextWordWrap,
        QStringLiteral("폭·길이는 모델 표면 투영값(m)입니다. 균열 유형의 cw는 영상 추정 두께 p90(mm)이며 실측값이 아닙니다."));
}

void append16(QByteArray *out, quint16 value) {
    out->append(char(value & 0xff)); out->append(char(value >> 8));
}
void append32(QByteArray *out, quint32 value) {
    append16(out, quint16(value)); append16(out, quint16(value >> 16));
}
quint32 crc32(const QByteArray &bytes) {
    quint32 crc = 0xffffffff;
    for (const unsigned char byte : bytes) {
        crc ^= byte;
        for (int i = 0; i < 8; ++i) crc = (crc >> 1) ^ (crc & 1 ? 0xedb88320u : 0u);
    }
    return ~crc;
}
bool writeXlsx(const QString &path, const QVector<const ProjectedDamage *> &damages) {
    QString xml = QStringLiteral("<?xml version=\"1.0\" encoding=\"UTF-8\" standalone=\"yes\"?>"
        "<worksheet xmlns=\"http://schemas.openxmlformats.org/spreadsheetml/2006/main\"><sheetData>");
    const QStringList headings{QStringLiteral("번호"), QStringLiteral("유형"),
        QStringLiteral("폭"), QStringLiteral("길이"), QStringLiteral("단위")};
    auto textCell = [&xml](int row, int col, const QString &value) {
        xml += QStringLiteral("<c r=\"%1%2\" t=\"inlineStr\"><is><t>%3</t></is></c>")
            .arg(QChar('A'+col)).arg(row).arg(value.toHtmlEscaped());
    };
    auto numberCell = [&xml](int row, int col, double value) {
        xml += QStringLiteral("<c r=\"%1%2\"><v>%3</v></c>")
            .arg(QChar('A'+col)).arg(row).arg(QString::number(value, 'f', 3));
    };
    xml += QStringLiteral("<row r=\"1\">");
    for (int col = 0; col < headings.size(); ++col) textCell(1, col, headings[col]);
    xml += QStringLiteral("</row>");
    for (int i = 0; i < damages.size(); ++i) {
        const auto &d = *damages[i]; const int row = i+2;
        xml += QStringLiteral("<row r=\"%1\">").arg(row);
        numberCell(row, 0, i+1); textCell(row, 1, quantityTypeLabel(d));
        if (d.boxWidthMm > 0) numberCell(row, 2, d.boxWidthMm / 1000.0);
        if (d.boxLengthMm > 0) numberCell(row, 3, d.boxLengthMm / 1000.0);
        textCell(row, 4, QStringLiteral("m"));
        xml += QStringLiteral("</row>");
    }
    xml += QStringLiteral("<row r=\"%1\">").arg(damages.size()+3);
    textCell(damages.size()+3, 0, QStringLiteral("투영 객체별 초안 · 중복 촬영 손상 미병합"));
    xml += QStringLiteral("</row>");
    xml += QStringLiteral("</sheetData></worksheet>");
    struct Entry { QByteArray name, data; quint32 crc = 0, offset = 0; };
    QVector<Entry> entries{
        {"[Content_Types].xml", "<?xml version=\"1.0\" encoding=\"UTF-8\"?>"
            "<Types xmlns=\"http://schemas.openxmlformats.org/package/2006/content-types\">"
            "<Default Extension=\"rels\" ContentType=\"application/vnd.openxmlformats-package.relationships+xml\"/>"
            "<Default Extension=\"xml\" ContentType=\"application/xml\"/>"
            "<Override PartName=\"/xl/workbook.xml\" ContentType=\"application/vnd.openxmlformats-officedocument.spreadsheetml.sheet.main+xml\"/>"
            "<Override PartName=\"/xl/worksheets/sheet1.xml\" ContentType=\"application/vnd.openxmlformats-officedocument.spreadsheetml.worksheet+xml\"/>"
            "</Types>"},
        {"_rels/.rels", "<?xml version=\"1.0\" encoding=\"UTF-8\"?>"
            "<Relationships xmlns=\"http://schemas.openxmlformats.org/package/2006/relationships\">"
            "<Relationship Id=\"rId1\" Type=\"http://schemas.openxmlformats.org/officeDocument/2006/relationships/officeDocument\" Target=\"xl/workbook.xml\"/>"
            "</Relationships>"},
        {"xl/workbook.xml", "<?xml version=\"1.0\" encoding=\"UTF-8\"?>"
            "<workbook xmlns=\"http://schemas.openxmlformats.org/spreadsheetml/2006/main\" "
            "xmlns:r=\"http://schemas.openxmlformats.org/officeDocument/2006/relationships\">"
            "<sheets><sheet name=\"Damage\" sheetId=\"1\" r:id=\"rId1\"/></sheets></workbook>"},
        {"xl/_rels/workbook.xml.rels", "<?xml version=\"1.0\" encoding=\"UTF-8\"?>"
            "<Relationships xmlns=\"http://schemas.openxmlformats.org/package/2006/relationships\">"
            "<Relationship Id=\"rId1\" Type=\"http://schemas.openxmlformats.org/officeDocument/2006/relationships/worksheet\" Target=\"worksheets/sheet1.xml\"/>"
            "</Relationships>"},
        {"xl/worksheets/sheet1.xml", xml.toUtf8()}
    };
    QSaveFile file(path);
    if (!file.open(QIODevice::WriteOnly)) return false;
    QByteArray archive, directory;
    for (auto &entry : entries) {
        entry.crc = crc32(entry.data); entry.offset = quint32(archive.size());
        append32(&archive, 0x04034b50); append16(&archive, 20); append16(&archive, 0);
        append16(&archive, 0); append16(&archive, 0); append16(&archive, 0);
        append32(&archive, entry.crc); append32(&archive, quint32(entry.data.size()));
        append32(&archive, quint32(entry.data.size())); append16(&archive, quint16(entry.name.size()));
        append16(&archive, 0); archive += entry.name; archive += entry.data;
        append32(&directory, 0x02014b50); append16(&directory, 20); append16(&directory, 20);
        append16(&directory, 0); append16(&directory, 0); append16(&directory, 0); append16(&directory, 0);
        append32(&directory, entry.crc); append32(&directory, quint32(entry.data.size()));
        append32(&directory, quint32(entry.data.size())); append16(&directory, quint16(entry.name.size()));
        append16(&directory, 0); append16(&directory, 0); append16(&directory, 0); append16(&directory, 0);
        append32(&directory, 0); append32(&directory, entry.offset); directory += entry.name;
    }
    const quint32 directoryOffset = quint32(archive.size()); archive += directory;
    append32(&archive, 0x06054b50); append16(&archive, 0); append16(&archive, 0);
    append16(&archive, quint16(entries.size())); append16(&archive, quint16(entries.size()));
    append32(&archive, quint32(directory.size())); append32(&archive, directoryOffset); append16(&archive, 0);
    return file.write(archive) == archive.size() && file.commit();
}

void fillUnrollScene(QGraphicsScene *scene, const UnrollChart &chart, const ProjectionBatch &batch,
                     const QString &selectedObject = {}) {
    scene->clear();
    for (const auto &strip : chart.strips) {
        const QRectF frame(strip.offset, -strip.top, strip.width, strip.top - strip.bottom);
        scene->addRect(frame, QPen(QColor(76, 99, 110), 0),
                       QBrush(strip.interior ? QColor(235, 244, 247) : QColor(249, 251, 251)));
        auto *label = scene->addText(strip.faceId.section(QLatin1Char('-'), -3));
        label->setDefaultTextColor(QColor(56, 74, 83));
        label->setScale(0.015);
        label->setPos(strip.offset + 0.03, -strip.top + 0.03);
    }
    for (const auto &item : batch.items) {
        if (item.partId != chart.partId || item.paths.isEmpty()) continue;
        const bool selected = item.objectId == selectedObject;
        QPen pen(selected ? QColor(0, 141, 112) : QColor(196, 52, 49), selected ? 3.5 : 2);
        pen.setCosmetic(true);
        for (const auto &line : item.paths) {
            QPainterPath path;
            bool valid = true;
            for (int i = 0; i < line.size(); ++i) {
                QPointF point;
                if (!chart.point(item.faceId, line[i], &point)) { valid = false; break; }
                if (i == 0) path.moveTo(point); else path.lineTo(point);
            }
            if (!valid || line.isEmpty()) continue;
            if (line.size() == 1) path.addEllipse(path.currentPosition(), 0.015, 0.015);
            auto *drawn = scene->addPath(path, pen);
            drawn->setZValue(selected ? 2 : 1);
            drawn->setToolTip(item.imageName + QStringLiteral(" · ") + item.faceId);
        }
    }
    if (!scene->items().isEmpty()) scene->setSceneRect(scene->itemsBoundingRect().adjusted(-0.3, -0.3, 0.3, 0.3));
}

class MapWindow : public QMainWindow {
public:
    MapWindow(const QString &projectDb, const QString &inspectionId) : projectDb_(projectDb), inspectionId_(inspectionId) {
        setWindowTitle(QStringLiteral("InspectMap | 외관망도 작성기"));
        resize(1600, 980);
        auto *root = new QWidget;
        auto *layout = new QVBoxLayout(root);
        layout->setContentsMargins(0, 0, 0, 0);
        layout->setSpacing(0);
        auto *title = new QLabel(QStringLiteral("InspectMap · 외관망도 작성기"));
        title->setObjectName(QStringLiteral("title"));
        layout->addWidget(title);
        auto *bar = new QWidget;
        auto *barLayout = new QHBoxLayout(bar);
        barLayout->setContentsMargins(18, 12, 18, 12);
        projectLabel_ = new QLabel;
        projectLabel_->setMinimumWidth(180);
        barLayout->addWidget(projectLabel_, 1);
        auto *open = new QPushButton(QStringLiteral("프로젝트 열기"));
        connect(open, &QPushButton::clicked, this, [this] {
            const QString file = QFileDialog::getOpenFileName(this, QStringLiteral("프로젝트 열기"), {},
                                                               QStringLiteral("SQLite (*.sqlite)"));
            if (!file.isEmpty()) { projectDb_ = file; inspectionId_.clear(); openProject(); }
        });
        barLayout->addWidget(open);
        dataset_ = new QComboBox;
        dataset_->setMinimumWidth(200);
        connect(dataset_, &QComboBox::currentIndexChanged, this, [this] { loadBatch(); });
        barLayout->addWidget(dataset_);
        mode_ = new QComboBox;
        mode_->addItem(QStringLiteral("부재별 방향도 · 기본"), QStringLiteral("map"));
        mode_->addItem(QStringLiteral("구간 측면 전개"), QStringLiteral("unroll"));
        connect(mode_, &QComboBox::currentIndexChanged, this, [this] { refreshParts(); });
        barLayout->addWidget(mode_);
        auto *run = new QPushButton(QStringLiteral("손상 3D 투영 실행"));
        run->setObjectName(QStringLiteral("primary"));
        connect(run, &QPushButton::clicked, this, [this] { runProjection(); });
        barLayout->addWidget(run);
        layout->addWidget(bar);

        auto *split = new QSplitter;
        auto *left = new QWidget;
        auto *leftLayout = new QVBoxLayout(left);
        leftLayout->setContentsMargins(16, 10, 8, 16);
        leftLayout->addWidget(new QLabel(QStringLiteral("부재")));
        parts_ = new QListWidget;
        connect(parts_, &QListWidget::currentRowChanged, this, [this] { refreshDrawings(); });
        leftLayout->addWidget(parts_, 1);
        leftLayout->addWidget(new QLabel(QStringLiteral("도면 방향")));
        drawings_ = new QListWidget;
        connect(drawings_, &QListWidget::currentRowChanged, this, [this] { refreshView(); });
        leftLayout->addWidget(drawings_, 1);
        split->addWidget(left);

        auto *center = new QWidget;
        auto *centerLayout = new QVBoxLayout(center);
        centerLayout->setContentsMargins(8, 10, 8, 16);
        auto *viewBar = new QHBoxLayout;
        drawingTitle_ = new QLabel(QStringLiteral("도면을 선택하십시오"));
        viewBar->addWidget(drawingTitle_, 1);
        auto *fit = new QPushButton(QStringLiteral("맞춤"));
        connect(fit, &QPushButton::clicked, this, [this] { view_->fit(); });
        viewBar->addWidget(fit);
        centerLayout->addLayout(viewBar);
        scene_ = new QGraphicsScene(this);
        view_ = new DrawingView;
        view_->pointPicked = [this](const QPointF &point) { addReviewPoint(point); };
        view_->setScene(scene_);
        centerLayout->addWidget(view_, 1);
        split->addWidget(center);

        auto *right = new QWidget;
        auto *rightLayout = new QVBoxLayout(right);
        rightLayout->setContentsMargins(8, 10, 16, 16);
        summary_ = new QLabel(QStringLiteral("투영 결과 없음"));
        summary_->setWordWrap(true);
        rightLayout->addWidget(summary_);
        rightLayout->addWidget(new QLabel(QStringLiteral("손상 물량표 · 폭·길이 m / 두께 mm")));
        items_ = new QTableWidget(0, 7);
        items_->setHorizontalHeaderLabels({QStringLiteral("번호"), QStringLiteral("유형"), QStringLiteral("폭(m)"),
            QStringLiteral("길이(m)"), QStringLiteral("두께(mm)"), QStringLiteral("이미지명"), QStringLiteral("비고")});
        items_->horizontalHeader()->setSectionResizeMode(QHeaderView::ResizeToContents);
        items_->horizontalHeader()->setStretchLastSection(true);
        items_->verticalHeader()->hide();
        items_->setSelectionBehavior(QAbstractItemView::SelectRows);
        items_->setEditTriggers(QAbstractItemView::NoEditTriggers);
        connect(items_, &QTableWidget::itemSelectionChanged, this, [this] {
            if (!capture_ && items_->currentRow() >= 0 && items_->item(items_->currentRow(), 0)) {
                reviewObjectId_ = items_->item(items_->currentRow(), 0)->data(Qt::UserRole).toString();
                updateReviewInfo();
            }
            if (drawings_->currentItem()) refreshView(false);
        });
        connect(items_, &QTableWidget::cellDoubleClicked, this, [this](int row) {
            if (row < 0 || !drawings_->currentItem() || !items_->item(row, 0)) return;
            const QString objectId = items_->item(row, 0)->data(Qt::UserRole).toString();
            if (isUnroll()) {
                QRectF bounds; bool hasPoint = false;
                const auto *sheet = engine_.unrollSheet(currentPart());
                if (!sheet) return;
                const QPointF shift = verifiedUnrollOffset(*sheet, engine_);
                for (const auto &item : batch_.items) if (item.objectId == objectId)
                    for (const auto &path : item.paths) for (const auto &world : path) {
                        QPointF point;
                        if (!sheet->point(item.faceId, world, &point)) continue;
                        const QRectF vertex(point+shift, QSizeF());
                        bounds = hasPoint ? bounds.united(vertex) : vertex; hasPoint = true;
                    }
                if (hasPoint) view_->fitInView(bounds.adjusted(-0.2, -0.2, 0.2, 0.2), Qt::KeepAspectRatio);
                return;
            }
            if (drawings_->currentItem()->data(Qt::UserRole).toString() == QStringLiteral("__MAP_SHEET__")) {
                for (const auto &item : batch_.items) if (item.objectId == objectId) {
                    for (int i = 0; i < drawings_->count(); ++i)
                        if (drawings_->item(i)->data(Qt::UserRole).toString() == item.drawingId) {
                            drawings_->setCurrentRow(i);
                            break;
                        }
                    break;
                }
            }
            const auto *drawing = engine_.drawing(drawings_->currentItem()->data(Qt::UserRole).toString());
            if (!drawing) return;
            QRectF bounds;
            bool hasPoint = false;
            for (const auto &item : batch_.items) {
                if (item.objectId != objectId) continue;
                for (const auto &path : item.paths) for (const auto &point : path) {
                    const QRectF vertex(InspectionMapEngine::drawingPoint(*drawing, point), QSizeF());
                    bounds = hasPoint ? bounds.united(vertex) : vertex;
                    hasPoint = true;
                }
            }
            if (hasPoint) view_->fitInView(bounds.adjusted(-0.2, -0.2, 0.2, 0.2), Qt::KeepAspectRatio);
        });
        rightLayout->addWidget(items_, 1);
        auto *quantityNote = new QLabel(QStringLiteral("* 두께는 영상 추정치이며 실측 개구폭이 아닙니다."));
        quantityNote->setWordWrap(true);
        rightLayout->addWidget(quantityNote);
        auto *reviewBox = new QGroupBox(QStringLiteral("투영 검토"));
        auto *reviewLayout = new QVBoxLayout(reviewBox);
        reviewInfo_ = new QLabel(QStringLiteral("손상을 선택하십시오."));
        reviewInfo_->setWordWrap(true); reviewInfo_->setMinimumHeight(76); reviewLayout->addWidget(reviewInfo_);
        reviewPart_ = new QComboBox; reviewPart_->setToolTip(QStringLiteral("수동 교정 대상 부재"));
        reviewDrawing_ = new QComboBox; reviewDrawing_->setToolTip(QStringLiteral("수동 교정 대상 방향도"));
        reviewFace_ = new QComboBox; reviewFace_->setToolTip(QStringLiteral("손상이 놓인 모델 면"));
        reviewLayout->addWidget(reviewPart_); reviewLayout->addWidget(reviewDrawing_);
        reviewLayout->addWidget(reviewFace_);
        connect(reviewPart_, &QComboBox::currentIndexChanged, this, [this] { updateReviewDrawings(); });
        connect(reviewDrawing_, &QComboBox::currentIndexChanged, this, [this] { updateReviewFaces(); });
        traceKind_ = new QComboBox;
        traceKind_->addItem(QStringLiteral("균열선 · 길이"), QStringLiteral("LINE"));
        traceKind_->addItem(QStringLiteral("영역 · 면적"), QStringLiteral("AREA"));
        reviewLayout->addWidget(traceKind_);
        auto *traceRow = new QHBoxLayout;
        auto *traceStart = new QPushButton(QStringLiteral("도면에 점 찍기"));
        connect(traceStart, &QPushButton::clicked, this, [this] { startReviewTrace(); });
        traceRow->addWidget(traceStart);
        auto *traceUndo = new QPushButton(QStringLiteral("↶"));
        traceUndo->setToolTip(QStringLiteral("마지막 점 취소"));
        connect(traceUndo, &QPushButton::clicked, this, [this] {
            if (!tracePoints_.isEmpty()) tracePoints_.removeLast(); refreshView(false);
        });
        traceRow->addWidget(traceUndo); reviewLayout->addLayout(traceRow);
        traceCount_ = new QLabel(QStringLiteral("점 0개")); reviewLayout->addWidget(traceCount_);
        auto *confirm = new QPushButton(QStringLiteral("위치·물량 확정"));
        connect(confirm, &QPushButton::clicked, this, [this] { confirmReviewTrace(); });
        reviewLayout->addWidget(confirm);
        auto *decisionRow = new QHBoxLayout;
        for (const auto &command : {QStringLiteral("제외"), QStringLiteral("보류"), QStringLiteral("되돌리기")}) {
            auto *button = new QPushButton(command);
            connect(button, &QPushButton::clicked, this, [this, command] {
                saveReviewDecision(command == QStringLiteral("제외") ? QStringLiteral("EXCLUDED") :
                    command == QStringLiteral("보류") ? QStringLiteral("DEFERRED") : QStringLiteral("RESET"));
            });
            decisionRow->addWidget(button);
        }
        reviewLayout->addLayout(decisionRow); rightLayout->addWidget(reviewBox);
        auto *exportButton = new QPushButton(QStringLiteral("부재 망도 PDF·물량표"));
        connect(exportButton, &QPushButton::clicked, this, [this] { exportPart(); });
        rightLayout->addWidget(exportButton);
        auto *htmlButton = new QPushButton(QStringLiteral("부재 망도 HTML"));
        connect(htmlButton, &QPushButton::clicked, this, [this] { exportPartHtml(); });
        rightLayout->addWidget(htmlButton);
        auto *excelButton = new QPushButton(QStringLiteral("손상 물량 Excel"));
        connect(excelButton, &QPushButton::clicked, this, [this] { exportPartXlsx(); });
        rightLayout->addWidget(excelButton);
        split->addWidget(right);
        split->setStretchFactor(0, 0); split->setStretchFactor(1, 1); split->setStretchFactor(2, 0);
        split->setSizes({230, 760, 650});
        layout->addWidget(split, 1);
        setCentralWidget(root);
        setStyleSheet(QStringLiteral(
            "QMainWindow,QWidget { background:#f4f7f8; color:#173044; font-size:14px; }"
            "QLabel#title { background:#243b4e; color:white; font-size:22px; font-weight:700; padding:18px 22px; }"
            "QListWidget,QTableWidget,QGraphicsView { background:white; border:1px solid #cbd6db; }"
            "QListWidget::item:selected,QTableWidget::item:selected { background:#dcefeb; color:#143b34; }"
            "QPushButton,QComboBox { background:white; border:1px solid #b9cbd0; padding:7px 12px; }"
            "QPushButton:hover { background:#e6f2ef; }"
            "QPushButton#primary { background:#137c70; color:white; border-color:#137c70; }"));
        if (!projectDb_.isEmpty()) openProject();
    }

    void runForBatch() { runProjection(false); }
    void setMode(const QString &mode) { mode_->setCurrentIndex(mode == QStringLiteral("unroll") ? 1 : 0); }
    void exportForBatch(const QString &part, const QString &path) { writePartPdf(part, path); }
    bool exportHtmlForBatch(const QString &part, const QString &path) { return writePartHtml(part, path); }
    bool exportXlsxForBatch(const QString &part, const QString &path) {
        const auto *sheet = isUnroll() ? engine_.unrollSheet(part) : nullptr;
        return writeXlsx(path, numberedDamages(batch_, part, sheet));
    }

private:
    bool isUnroll() const { return mode_->currentData().toString() == QStringLiteral("unroll"); }
    QString currentPart() const { return parts_->currentItem() ? parts_->currentItem()->data(Qt::UserRole).toString() : QString(); }
    bool openProject() {
        projectLabel_->setText(QFileInfo(projectDb_).absolutePath());
        const QString connection = QStringLiteral("map-editor-discovery");
        QString root;
        {
            auto db = QSqlDatabase::addDatabase(QStringLiteral("QSQLITE"), connection);
            db.setDatabaseName(projectDb_);
            if (!db.open()) { QMessageBox::warning(this, QStringLiteral("프로젝트 열기 실패"), db.lastError().text()); return false; }
            QSqlQuery query(db);
            query.prepare(QStringLiteral("SELECT inspection_id,model_root FROM registration_result "
                "WHERE active=1 AND review_status='CONFIRMED' AND (?='' OR inspection_id=?) "
                "ORDER BY confirmed_at DESC LIMIT 1"));
            const QString scope = inspectionId_.isEmpty() ? QStringLiteral("") : inspectionId_;
            query.addBindValue(scope); query.addBindValue(scope);
            if (query.exec() && query.next()) {
                inspectionId_ = query.value(0).toString(); root = query.value(1).toString();
            }
            db.close();
        }
        QSqlDatabase::removeDatabase(connection);
        if (root.isEmpty()) {
            QMessageBox::information(this, QStringLiteral("정합 결과 필요"),
                QStringLiteral("검토 확정된 모델 정합 결과가 있어야 망도 작업을 시작할 수 있습니다."));
            return false;
        }
        QString error;
        if (!engine_.load(root, &error)) {
            QMessageBox::warning(this, QStringLiteral("모델·도면 읽기 실패"), error); return false;
        }
        if (auto *model = qobject_cast<QStandardItemModel *>(mode_->model()))
            model->item(1)->setEnabled(!engine_.unrollSheets().isEmpty());
        if (engine_.unrollSheets().isEmpty() && isUnroll()) mode_->setCurrentIndex(0);
        reviewPart_->blockSignals(true); reviewPart_->clear();
        for (const auto &part : engine_.parts()) reviewPart_->addItem(part, part);
        reviewPart_->blockSignals(false); updateReviewDrawings();
        QList<DamageDataset> datasets;
        if (!DamageStore::datasets(projectDb_, inspectionId_, &datasets, &error)) {
            QMessageBox::warning(this, QStringLiteral("데이터 세트 읽기 실패"), error); return false;
        }
        dataset_->blockSignals(true); dataset_->clear();
        for (const auto &dataset : datasets)
            dataset_->addItem(QStringLiteral("%1 · %2장").arg(dataset.name).arg(dataset.imageCount), dataset.id);
        dataset_->blockSignals(false);
        loadBatch();
        statusBar()->showMessage(QStringLiteral("모델 %1개 부재 · 방향도 %2개")
            .arg(engine_.parts().size()).arg(engine_.drawings().size()), 5000);
        return true;
    }

    void loadBatch() {
        batch_ = {};
        if (!projectDb_.isEmpty() && !inspectionId_.isEmpty() && dataset_->currentIndex() >= 0) {
            QString error;
            if (!InspectionMapWorkflow::active(projectDb_, inspectionId_, dataset_->currentData().toString(),
                                               &batch_, &error)) statusBar()->showMessage(error);
        }
        refreshParts();
        updateReviewInfo();
    }

    void updateReviewDrawings() {
        reviewDrawing_->blockSignals(true); reviewDrawing_->clear();
        for (const auto &drawing : engine_.drawings())
            if (drawing.partId == reviewPart_->currentData().toString())
                reviewDrawing_->addItem(drawing.id, drawing.id);
        reviewDrawing_->blockSignals(false); updateReviewFaces();
    }

    void updateReviewFaces() {
        reviewFace_->clear();
        const auto *drawing = engine_.drawing(reviewDrawing_->currentData().toString());
        if (!drawing) return;
        QStringList faces = drawing->faceIds.values(); faces.sort();
        for (const auto &face : faces) reviewFace_->addItem(face, face);
    }

    const ProjectedDamage *reviewItem() const {
        for (const auto &item : batch_.items) if (item.objectId == reviewObjectId_) return &item;
        return nullptr;
    }

    void updateReviewInfo() {
        const auto *item = reviewItem();
        if (!item) { reviewInfo_->setText(QStringLiteral("손상을 선택하십시오.")); return; }
        reviewInfo_->setText(QStringLiteral("%1 · %2\n%3\n판정: %4")
            .arg(item->imageName, typeLabel(item->type), item->reason,
                 item->reviewState.isEmpty() ? QStringLiteral("미검토") :
                 item->reviewState == QStringLiteral("EXCLUDED") ? QStringLiteral("제외") :
                 item->reviewState == QStringLiteral("DEFERRED") ? QStringLiteral("보류") : QStringLiteral("수동 확정")));
        const int partIndex = reviewPart_->findData(item->partId);
        if (partIndex >= 0) reviewPart_->setCurrentIndex(partIndex);
        const int drawingIndex = reviewDrawing_->findData(item->drawingId);
        if (drawingIndex >= 0) reviewDrawing_->setCurrentIndex(drawingIndex);
        const int faceIndex = reviewFace_->findData(item->faceId);
        if (faceIndex >= 0) reviewFace_->setCurrentIndex(faceIndex);
        traceKind_->setCurrentIndex(item->type == QStringLiteral("crack") ? 0 : 1);
        showReviewImage(*item);
    }

    void showReviewImage(const ProjectedDamage &item) {
        if (currentPart() != QStringLiteral("__REVIEW__") &&
            currentPart() != QStringLiteral("__EXCLUDED__")) return;
        QList<DamageImage> images;
        QString error;
        if (!DamageStore::images(projectDb_, dataset_->currentData().toString(), &images, &error)) return;
        const auto imageIt = std::find_if(images.cbegin(), images.cend(), [&](const auto &image) {
            return image.id == item.imageId;
        });
        if (imageIt == images.cend()) return;
        QImageReader reader(imageIt->path);
        reader.setAutoTransform(true);
        const QTransform orientation = previewOrientation(reader.transformation(),
            QSize(imageIt->width, imageIt->height));
        const QImage pixels = reader.read();
        if (pixels.isNull()) { statusBar()->showMessage(QStringLiteral("검토 영상 없음: ") + imageIt->path); return; }
        scene_->clear();
        scene_->addPixmap(QPixmap::fromImage(pixels))->setZValue(-1);
        QRectF focus;
        QList<DamageObject> objects;
        if (DamageStore::objects(projectDb_, imageIt->resultId, &objects, &error))
            for (const auto &object : objects) if (object.id == item.objectId) {
                auto addPixels = [this, &orientation](const QJsonArray &pixels, bool closed) {
                    QPainterPath path;
                    for (int i = 0; i < pixels.size(); ++i) {
                        const auto point = pixels[i].toArray();
                        if (point.size() < 2) continue;
                        const QPointF xy(point[0].toDouble(), point[1].toDouble());
                        if (path.isEmpty()) path.moveTo(xy); else path.lineTo(xy);
                    }
                    if (closed) path.closeSubpath();
                    scene_->addPath(orientation.map(path), QPen(QColor(235, 65, 52), 3))->setZValue(2);
                };
                for (const auto &line : object.geometry.value(QStringLiteral("lines")).toArray())
                    addPixels(line.toArray(), false);
                for (const auto &ring : object.geometry.value(QStringLiteral("polygon")).toArray())
                    addPixels(ring.toArray(), true);
                for (const auto &region : object.geometry.value(QStringLiteral("regions")).toArray())
                    for (const auto &ring : region.toObject().value(QStringLiteral("polygon")).toArray())
                        addPixels(ring.toArray(), true);
                if (object.bbox.size() == 4) {
                    const QRectF box(QPointF(object.bbox[0].toDouble(), object.bbox[1].toDouble()),
                                     QPointF(object.bbox[2].toDouble(), object.bbox[3].toDouble()));
                    QPainterPath outline; outline.addRect(box.normalized());
                    const QPainterPath mapped = orientation.map(outline);
                    focus = mapped.boundingRect();
                    scene_->addPath(mapped,
                                    QPen(QColor(0, 210, 110), 2, Qt::DashLine))->setZValue(3);
                }
                break;
            }
        scene_->setSceneRect(QRectF(QPointF(0, 0), pixels.size()));
        drawingTitle_->setText(item.imageName + QStringLiteral(" · 투영 검토"));
        if (focus.isValid() && !focus.isEmpty())
            view_->fitInView(focus.adjusted(-300, -300, 300, 300).intersected(scene_->sceneRect()),
                             Qt::KeepAspectRatio);
        else view_->fit();
    }

    void startReviewTrace() {
        if (!reviewItem()) {
            QMessageBox::information(this, QStringLiteral("손상 선택"), QStringLiteral("검토할 손상을 먼저 선택하십시오.")); return;
        }
        const QString part = reviewPart_->currentData().toString();
        const QString drawing = reviewDrawing_->currentData().toString();
        if (part.isEmpty() || drawing.isEmpty() || reviewFace_->currentData().toString().isEmpty()) {
            QMessageBox::information(this, QStringLiteral("대상 지정"), QStringLiteral("부재·도면·면을 선택하십시오.")); return;
        }
        capture_ = true; view_->capturePoints = true; tracePoints_.clear();
        if (isUnroll()) mode_->setCurrentIndex(0);
        for (int i = 0; i < parts_->count(); ++i) if (parts_->item(i)->data(Qt::UserRole) == part) {
            parts_->setCurrentRow(i); break;
        }
        for (int i = 0; i < drawings_->count(); ++i) if (drawings_->item(i)->data(Qt::UserRole) == drawing) {
            drawings_->setCurrentRow(i); break;
        }
        traceCount_->setText(QStringLiteral("도면에서 점을 찍으십시오 · 점 0개"));
    }

    void addReviewPoint(const QPointF &point) {
        if (!capture_) return;
        const QString drawing = reviewDrawing_->currentData().toString();
        const QString face = reviewFace_->currentData().toString();
        if (!drawings_->currentItem() || drawings_->currentItem()->data(Qt::UserRole).toString() != drawing) return;
        QVector3D world;
        if (!engine_.mapPoint(drawing, face, point, &world)) {
            statusBar()->showMessage(QStringLiteral("선택한 모델 면 안의 점을 지정하십시오."), 4000); return;
        }
        tracePoints_.append(world);
        traceCount_->setText(QStringLiteral("점 %1개 · %2").arg(tracePoints_.size()).arg(face));
        refreshView(false);
    }

    void drawReviewTrace(const MapDrawing &drawing) {
        if (!capture_ || drawing.id != reviewDrawing_->currentData().toString()) return;
        QPainterPath path;
        for (int i = 0; i < tracePoints_.size(); ++i) {
            const QPointF point = InspectionMapEngine::drawingPoint(drawing, tracePoints_[i]);
            if (i == 0) path.moveTo(point); else path.lineTo(point);
            scene_->addEllipse(QRectF(point.x()-0.08, point.y()-0.08, 0.16, 0.16),
                               QPen(QColor(0, 120, 95), 0), QBrush(Qt::white))->setZValue(10);
        }
        scene_->addPath(path, QPen(QColor(0, 120, 95), 2, Qt::DashLine))->setZValue(9);
    }

    void saveReviewDecision(const QString &decision, const ProjectedDamage *corrected = nullptr) {
        if (!reviewItem()) {
            QMessageBox::information(this, QStringLiteral("손상 선택"), QStringLiteral("검토할 손상을 먼저 선택하십시오.")); return;
        }
        QString error;
        if (!InspectionMapWorkflow::review(projectDb_, batch_.runId, reviewObjectId_, engine_, decision, corrected, &error)) {
            QMessageBox::warning(this, QStringLiteral("검토 저장 실패"), error); return;
        }
        capture_ = false; view_->capturePoints = false; tracePoints_.clear();
        traceCount_->setText(QStringLiteral("점 0개"));
        loadBatch();
        statusBar()->showMessage(QStringLiteral("검토 판정을 저장했습니다."), 5000);
    }

    void confirmReviewTrace() {
        const bool area = traceKind_->currentData().toString() == QStringLiteral("AREA");
        if (!capture_ || tracePoints_.size() < (area ? 3 : 2) || !reviewItem()) {
            QMessageBox::information(this, QStringLiteral("형상 필요"),
                area ? QStringLiteral("영역의 점을 3개 이상 찍으십시오.") : QStringLiteral("선의 점을 2개 이상 찍으십시오."));
            return;
        }
        ProjectedDamage corrected = *reviewItem();
        corrected.partId = reviewPart_->currentData().toString();
        corrected.drawingId = reviewDrawing_->currentData().toString();
        corrected.faceId = reviewFace_->currentData().toString();
        corrected.paths = {tracePoints_}; corrected.lengthMm = 0; corrected.areaMm2 = 0;
        if (area) {
            QVector3D cross;
            for (int i = 0; i < tracePoints_.size(); ++i)
                cross += QVector3D::crossProduct(tracePoints_[i], tracePoints_[(i+1)%tracePoints_.size()]);
            corrected.areaMm2 = 500000.0 * double(cross.length());
        } else for (int i = 1; i < tracePoints_.size(); ++i)
            corrected.lengthMm += 1000.0 * double((tracePoints_[i]-tracePoints_[i-1]).length());
        if (corrected.lengthMm <= 0 && corrected.areaMm2 <= 0) {
            QMessageBox::warning(this, QStringLiteral("물량 계산 불가"), QStringLiteral("점들이 중복되거나 영역이 유효하지 않습니다.")); return;
        }
        saveReviewDecision(QStringLiteral("CORRECTED"), &corrected);
    }

    void runProjection(bool notify = true) {
        if (dataset_->currentIndex() < 0) {
            QMessageBox::information(this, QStringLiteral("데이터 세트 필요"),
                QStringLiteral("손상 결과가 등록된 데이터 세트를 선택하십시오.")); return;
        }
        QString error;
        QApplication::setOverrideCursor(Qt::WaitCursor);
        ProjectionBatch next;
        const bool ok = InspectionMapWorkflow::run(projectDb_, inspectionId_, dataset_->currentData().toString(),
                                                   engine_, &next, &error);
        QApplication::restoreOverrideCursor();
        if (!ok) { QMessageBox::warning(this, QStringLiteral("투영 실패"), error); return; }
        batch_ = std::move(next);
        refreshParts();
        for (int row = 0; row < parts_->count(); ++row) {
            const QString part = parts_->item(row)->data(Qt::UserRole).toString();
            const auto *sheet = isUnroll() ? engine_.unrollSheet(part) : nullptr;
            if (std::any_of(batch_.items.cbegin(), batch_.items.cend(), [&](const auto &item) {
                return (sheet ? sheet->parts.contains(item.partId) &&
                    (sheet->side == QStringLiteral("GEN") ||
                     item.faceId.section(QLatin1Char('-'), 4, 4) == sheet->side) : item.partId == part) &&
                    item.status == QStringLiteral("PROJECTED");
            })) { parts_->setCurrentRow(row); break; }
        }
        for (int row = 0; row < drawings_->count(); ++row) {
            const QString drawing = drawings_->item(row)->data(Qt::UserRole).toString();
            if (std::any_of(batch_.items.cbegin(), batch_.items.cend(), [&](const auto &item) {
                return item.drawingId == drawing && item.status == QStringLiteral("PROJECTED");
            })) { drawings_->setCurrentRow(row); break; }
        }
        int done = 0, review = 0;
        for (const auto &item : batch_.items)
            item.status == QStringLiteral("PROJECTED") ? ++done : ++review;
        if (notify) QMessageBox::information(this, QStringLiteral("투영 완료"),
            QStringLiteral("도면 투영 %1건 · 확인 필요/불가 %2건\n검토 확정된 채택 손상만 처리했습니다.")
                .arg(done).arg(review));
    }

    void refreshParts() {
        const QString previous = parts_->currentItem() ? parts_->currentItem()->data(Qt::UserRole).toString() : QString();
        parts_->blockSignals(true); parts_->clear();
        if (isUnroll()) for (const auto &sheet : engine_.unrollSheets()) {
            const int count = std::count_if(batch_.items.cbegin(), batch_.items.cend(), [&](const auto &item) {
                return sheet.parts.contains(item.partId) &&
                    (sheet.side == QStringLiteral("GEN") ||
                     item.faceId.section(QLatin1Char('-'), 4, 4) == sheet.side) &&
                    item.status == QStringLiteral("PROJECTED");
            });
            auto *entry = new QListWidgetItem(QStringLiteral("%1 · %2 · %3구간 · %4건")
                .arg(sheet.series).arg(sheet.side == QStringLiteral("EX") ? QStringLiteral("외측") :
                    sheet.side == QStringLiteral("IN") ? QStringLiteral("내측") : QStringLiteral("무구분"))
                .arg(sheet.parts.size()).arg(count));
            entry->setData(Qt::UserRole, sheet.id);
            parts_->addItem(entry);
        }
        else for (const auto &part : engine_.parts()) {
            const int count = std::count_if(batch_.items.cbegin(), batch_.items.cend(),
                [&](const auto &item) { return item.partId == part; });
            auto *item = new QListWidgetItem(QStringLiteral("%1  ·  %2건").arg(part).arg(count));
            item->setData(Qt::UserRole, part);
            parts_->addItem(item);
        }
        const int unresolved = std::count_if(batch_.items.cbegin(), batch_.items.cend(),
            [](const auto &item) { return item.status != QStringLiteral("PROJECTED") &&
                item.status != QStringLiteral("EXCLUDED"); });
        if (unresolved > 0 && !isUnroll()) {
            auto *item = new QListWidgetItem(QStringLiteral("확인 필요  ·  %1건").arg(unresolved));
            item->setData(Qt::UserRole, QStringLiteral("__REVIEW__"));
            item->setForeground(QColor(145, 91, 15));
            parts_->addItem(item);
        }
        const int excluded = std::count_if(batch_.items.cbegin(), batch_.items.cend(),
            [](const auto &item) { return item.status == QStringLiteral("EXCLUDED"); });
        if (excluded > 0 && !isUnroll()) {
            auto *item = new QListWidgetItem(QStringLiteral("검토 제외  ·  %1건").arg(excluded));
            item->setData(Qt::UserRole, QStringLiteral("__EXCLUDED__")); parts_->addItem(item);
        }
        parts_->blockSignals(false);
        int row = 0;
        for (int i = 0; i < parts_->count(); ++i)
            if (parts_->item(i)->data(Qt::UserRole).toString() == previous) { row = i; break; }
        if (parts_->count() > 0) parts_->setCurrentRow(row);
        const int done = std::count_if(batch_.items.cbegin(), batch_.items.cend(),
            [](const auto &item) { return item.status == QStringLiteral("PROJECTED"); });
        summary_->setText(QStringLiteral("채택 손상 %1건 · 투영 %2건 · 확인 필요/불가 %3건 · 제외 %4건")
            .arg(batch_.items.size()).arg(done).arg(unresolved).arg(excluded));
    }

    void refreshDrawings() {
        const QString part = currentPart();
        drawings_->blockSignals(true); drawings_->clear();
        if (part == QStringLiteral("__REVIEW__") || part == QStringLiteral("__EXCLUDED__")) {
            drawings_->blockSignals(false);
            scene_->clear(); drawingTitle_->setText(part == QStringLiteral("__REVIEW__") ?
                QStringLiteral("확인 필요") : QStringLiteral("검토 제외"));
            fillItems(part, true);
            return;
        }
        if (isUnroll() && !part.isEmpty()) {
            auto *item = new QListWidgetItem(QStringLiteral("구간 전체 전개"));
            item->setData(Qt::UserRole, QStringLiteral("__UNROLL__"));
            drawings_->addItem(item);
            drawings_->blockSignals(false);
            drawings_->setCurrentRow(0);
            return;
        }
        if (!part.isEmpty()) {
            auto *item = new QListWidgetItem(QStringLiteral("전체 배치"));
            item->setData(Qt::UserRole, QStringLiteral("__MAP_SHEET__"));
            drawings_->addItem(item);
        }
        for (const auto &drawing : engine_.drawings()) {
            if (drawing.partId != part) continue;
            auto *item = new QListWidgetItem(drawing.id.mid(part.size() + 1));
            item->setData(Qt::UserRole, drawing.id);
            drawings_->addItem(item);
        }
        drawings_->blockSignals(false);
        if (drawings_->count() > 0) drawings_->setCurrentRow(0);
        else { scene_->clear(); items_->setRowCount(0); }
    }

    void fillItems(const QString &drawingId, bool unresolvedOnly = false) {
        items_->blockSignals(true); items_->setRowCount(0);
        const auto *sheet = drawingId == QStringLiteral("__UNROLL__") ? engine_.unrollSheet(currentPart()) : nullptr;
        const auto *drawing = engine_.drawing(drawingId);
        const auto numbered = numberedDamages(batch_, drawing ? drawing->partId : currentPart(), sheet);
        QVector<const ProjectedDamage *> visible;
        for (const auto &item : batch_.items) {
            if (unresolvedOnly ? (drawingId == QStringLiteral("__EXCLUDED__") ?
                    item.status != QStringLiteral("EXCLUDED") :
                    item.status == QStringLiteral("PROJECTED") || item.status == QStringLiteral("EXCLUDED")) :
                sheet ? (!sheet->parts.contains(item.partId) ||
                         (sheet->side != QStringLiteral("GEN") &&
                          item.faceId.section(QLatin1Char('-'), 4, 4) != sheet->side)) :
                drawingId == QStringLiteral("__MAP_SHEET__") ? item.partId != currentPart() : item.drawingId != drawingId)
                continue;
            visible.append(&item);
        }
        std::sort(visible.begin(), visible.end(), [&](const auto *a, const auto *b) {
            const int aNumber = damageNumber(numbered, *a);
            const int bNumber = damageNumber(numbered, *b);
            if (aNumber && bNumber) return aNumber < bNumber;
            if (aNumber != bNumber) return aNumber > 0;
            if (a->imageName != b->imageName) return a->imageName < b->imageName;
            return a->objectId < b->objectId;
        });
        for (const auto *visibleItem : visible) {
            const auto &item = *visibleItem;
            const int row = items_->rowCount(); items_->insertRow(row);
            const int number = damageNumber(numbered, item);
            items_->setItem(row, 0, new QTableWidgetItem(number > 0 ? QString::number(number) : QStringLiteral("—")));
            items_->setItem(row, 1, new QTableWidgetItem(typeLabel(item.type)));
            const bool excluded = sheet && item.status == QStringLiteral("PROJECTED") &&
                std::none_of(sheet->faces.cbegin(), sheet->faces.cend(),
                    [&](const auto &face) { return face.faceId == item.faceId; });
            const bool measured = item.status == QStringLiteral("PROJECTED");
            items_->setItem(row, 2, new QTableWidgetItem(metreLabel(measured ? item.boxWidthMm : 0)));
            items_->setItem(row, 3, new QTableWidgetItem(metreLabel(measured ? item.boxLengthMm : 0)));
            items_->setItem(row, 4, new QTableWidgetItem(dimensionLabel(measured ? item.crackP90Mm : 0)));
            items_->setItem(row, 5, new QTableWidgetItem(item.imageName));
            const QString note = excluded ? QStringLiteral("전개 제외") :
                item.reviewState == QStringLiteral("DEFERRED") ? QStringLiteral("보류") : statusLabel(item.status);
            items_->setItem(row, 6, new QTableWidgetItem(note));
            for (int col = 0; col < 7; ++col) {
                items_->item(row, col)->setData(Qt::UserRole, item.objectId);
                items_->item(row, col)->setToolTip(item.imageName + QStringLiteral("\n") +
                    item.faceId + QStringLiteral("\n") + item.reason);
            }
        }
        items_->blockSignals(false);
    }

    void refreshView(bool fit = true) {
        const QString id = drawings_->currentItem() ? drawings_->currentItem()->data(Qt::UserRole).toString() : QString();
        if (id == QStringLiteral("__UNROLL__")) {
            const auto *sheet = engine_.unrollSheet(currentPart());
            if (!sheet) { scene_->clear(); items_->setRowCount(0); return; }
            const int excludedDamage = std::count_if(batch_.items.cbegin(), batch_.items.cend(), [&](const auto &item) {
                return sheet->parts.contains(item.partId) &&
                    (sheet->side == QStringLiteral("GEN") ||
                     item.faceId.section(QLatin1Char('-'), 4, 4) == sheet->side) &&
                    item.status == QStringLiteral("PROJECTED") &&
                    std::none_of(sheet->faces.cbegin(), sheet->faces.cend(),
                        [&](const auto &face) { return face.faceId == item.faceId; });
            });
            const QString sideName = sheet->side == QStringLiteral("EX") ? QStringLiteral("외측") :
                sheet->side == QStringLiteral("IN") ? QStringLiteral("내측") : QStringLiteral("무구분");
            drawingTitle_->setText(QStringLiteral("%1 · %2 전개 · %3구간 · 면 %4 · 전개 제외 손상 %5 · %6")
                .arg(sheet->series, sideName).arg(sheet->parts.size()).arg(sheet->faces.size())
                .arg(excludedDamage).arg(sheet->sourceDxf ? QStringLiteral("기준 DXF") : QStringLiteral("모델 자동 생성")));
            if (fit) fillItems(id);
            const QString selected = items_->currentRow() >= 0 && items_->item(items_->currentRow(), 0) ?
                items_->item(items_->currentRow(), 0)->data(Qt::UserRole).toString() : QString();
            Q_UNUSED(selected);
            fillVerifiedUnrollScene(scene_, *sheet, engine_, batch_);
            if (fit) view_->fit();
            return;
        }
        if (id == QStringLiteral("__MAP_SHEET__")) {
            drawingTitle_->setText(currentPart() + QStringLiteral(" · 방향도 전체 배치"));
            if (fit) fillItems(id);
            fillMapSheetScene(scene_, currentPart(), engine_, batch_);
            if (fit) view_->fit();
            return;
        }
        const auto *drawing = engine_.drawing(id);
        if (!drawing) { scene_->clear(); items_->setRowCount(0); return; }
        drawingTitle_->setText(drawing->id);
        if (fit) fillItems(drawing->id);
        const QString selected = items_->currentRow() >= 0 && items_->item(items_->currentRow(), 0) ?
            items_->item(items_->currentRow(), 0)->data(Qt::UserRole).toString() : QString();
        fillScene(scene_, *drawing, batch_, selected);
        drawReviewTrace(*drawing);
        if (fit) view_->fit();
    }

    void exportPart() {
        const QString part = parts_->currentItem() ? parts_->currentItem()->data(Qt::UserRole).toString() : QString();
        if (part.isEmpty() || part == QStringLiteral("__REVIEW__")) return;
        const QString path = QFileDialog::getSaveFileName(this, QStringLiteral("부재 망도 PDF 초안"),
            QDir(QFileInfo(projectDb_).absolutePath()).filePath(QString(part).replace(QLatin1Char('|'), QLatin1Char('_')) + QStringLiteral("_망도초안.pdf")),
            QStringLiteral("PDF (*.pdf)"));
        if (path.isEmpty()) return;
        writePartPdf(part, path);
    }

    void exportPartHtml() {
        const QString part = currentPart(); if (part.isEmpty()) return;
        const QString path = QFileDialog::getSaveFileName(this, QStringLiteral("부재 망도 HTML"),
            QDir(QFileInfo(projectDb_).absolutePath()).filePath(QString(part).replace(QLatin1Char('|'), QLatin1Char('_')) + QStringLiteral("_망도.html")),
            QStringLiteral("HTML (*.html)"));
        if (path.isEmpty()) return;
        if (!writePartHtml(part, path)) QMessageBox::warning(this, QStringLiteral("HTML 출력 실패"), path);
        else statusBar()->showMessage(QStringLiteral("HTML 출력: ") + path, 6000);
    }

    void exportPartXlsx() {
        const QString part = currentPart(); if (part.isEmpty()) return;
        const QString path = QFileDialog::getSaveFileName(this, QStringLiteral("손상 물량 Excel"),
            QDir(QFileInfo(projectDb_).absolutePath()).filePath(QString(part).replace(QLatin1Char('|'), QLatin1Char('_')) + QStringLiteral("_물량.xlsx")),
            QStringLiteral("Excel (*.xlsx)"));
        if (path.isEmpty()) return;
        if (!exportXlsxForBatch(part, path)) QMessageBox::warning(this, QStringLiteral("Excel 출력 실패"), path);
        else statusBar()->showMessage(QStringLiteral("Excel 출력: ") + path, 6000);
    }

    bool writePartHtml(const QString &part, const QString &path) {
        if (part.isEmpty() || path.isEmpty()) return false;
        const auto *sheet = isUnroll() ? engine_.unrollSheet(part) : nullptr;
        if (isUnroll() && !sheet) return false;
        QGraphicsScene drawingScene;
        if (sheet) fillVerifiedUnrollScene(&drawingScene, *sheet, engine_, batch_);
        else fillMapSheetScene(&drawingScene, part, engine_, batch_);
        QImage image(2600, 1700, QImage::Format_ARGB32_Premultiplied);
        image.fill(Qt::white);
        { QPainter painter(&image); painter.setRenderHint(QPainter::Antialiasing);
          drawingScene.render(&painter, QRectF(30, 30, 2540, 1640), drawingScene.sceneRect(), Qt::KeepAspectRatio); }
        QByteArray png; QBuffer buffer(&png);
        if (!buffer.open(QIODevice::WriteOnly) || !image.save(&buffer, "PNG")) return false;
        const auto damages = numberedDamages(batch_, part, sheet);
        QString html = QStringLiteral("<!doctype html><html lang=\"ko\"><head><meta charset=\"utf-8\">"
            "<title>%1 외관조사망도</title><style>body{font:14px sans-serif;color:#193144;margin:28px}"
            "h1{font-size:24px}.sheet{display:grid;grid-template-columns:minmax(0,1fr) minmax(420px,32%);gap:20px;align-items:start}"
            "img{display:block;width:100%;border:1px solid #ccd5d9}table{border-collapse:collapse;width:100%;font-size:12px}"
            "th,td{border:1px solid #cdd6d9;padding:6px 8px;text-align:left}th{background:#e8eff0}"
            ".note{color:#526b78;font-size:12px}@media(max-width:1100px){.sheet{grid-template-columns:1fr}}"
            "@media print{@page{size:A1 landscape}body{margin:12mm}.sheet{grid-template-columns:minmax(0,1fr) 32%}tr{break-inside:avoid}}"
            "</style></head><body><h1>%1 외관조사망도</h1><p>투영 객체별 초안 · 중복 촬영 미병합 · 3D 투영 물량</p>")
            .arg(part.toHtmlEscaped());
        html += QStringLiteral("<div class=\"sheet\"><img alt=\"부재 망도\" src=\"data:image/png;base64,%1\">")
            .arg(QString::fromLatin1(png.toBase64()));
        html += QStringLiteral("<aside><h2>손상 물량표</h2><table><thead><tr><th>번호</th><th>유형</th>"
            "<th>폭</th><th>길이</th><th>단위</th>"
            "</tr></thead><tbody>");
        for (int i = 0; i < damages.size(); ++i) {
            const auto &d = *damages[i];
            html += QStringLiteral("<tr><td>%1</td><td>%2</td><td>%3</td><td>%4</td><td>m</td></tr>")
                .arg(i+1).arg(quantityTypeLabel(d).toHtmlEscaped(), metreLabel(d.boxWidthMm),
                    metreLabel(d.boxLengthMm));
        }
        html += QStringLiteral("</tbody></table><p class=\"note\">폭·길이는 모델 표면 투영값(m)입니다. 균열 유형의 cw는 영상 추정 두께 p90(mm)이며 실측값이 아닙니다.</p></aside></div></body></html>");
        QSaveFile file(path);
        if (!file.open(QIODevice::WriteOnly)) return false;
        const QByteArray bytes = html.toUtf8();
        return file.write(bytes) == bytes.size() && file.commit();
    }

    void writePartPdf(const QString &part, const QString &path) {
        if (part.isEmpty() || path.isEmpty()) return;
        const UnrollSheet *sheet = isUnroll() ? engine_.unrollSheet(part) : nullptr;
        if (isUnroll() && !sheet) {
            QMessageBox::warning(this, QStringLiteral("전개도 없음"), QStringLiteral("검증 가능한 전개 면이 없습니다."));
            return;
        }
        QPdfWriter pdf(path);
        pdf.setPageSize(QPageSize(QPageSize::A1));
        pdf.setPageOrientation(QPageLayout::Landscape);
        pdf.setResolution(72);
        QPainter painter(&pdf);
        if (!painter.isActive()) {
            QMessageBox::warning(this, QStringLiteral("PDF 출력 실패"), path); return;
        }
        const QRect page = painter.viewport();
        painter.fillRect(page, Qt::white);
        QFont titleFont = painter.font(); titleFont.setPixelSize(30); titleFont.setBold(true);
        painter.setFont(titleFont); painter.setPen(QColor(30, 53, 68));
        painter.drawText(QRectF(40, 25, page.width() - 80, 45), part + QStringLiteral("  외관조사망도 초안"));
        QFont detailFont = painter.font(); detailFont.setPixelSize(15); detailFont.setBold(false);
        painter.setFont(detailFont);
        const int unresolved = std::count_if(batch_.items.cbegin(), batch_.items.cend(),
            [&](const auto &item) { return (sheet ? sheet->parts.contains(item.partId) &&
                (sheet->side == QStringLiteral("GEN") ||
                 item.faceId.section(QLatin1Char('-'), 4, 4) == sheet->side) : item.partId == part) &&
                item.status != QStringLiteral("PROJECTED") && item.status != QStringLiteral("EXCLUDED"); });
        painter.drawText(QRectF(40, 70, page.width() - 80, 25),
            QStringLiteral("정합 %1 · 확인 필요 %2건 · 외접 치수는 3D 투영값, 두께는 영상 추정값")
                .arg(batch_.registrationId.left(8)).arg(unresolved));
        const int tableLeft = int(page.width() * 0.70);
        const QRect tableArea(tableLeft, 135, page.width()-tableLeft-40, page.height()-195);
        if (sheet) {
            painter.drawText(QRectF(40, 95, page.width() - 80, 25),
                QStringLiteral("측면 전개 · %1구간 · 면 %2 · %3")
                    .arg(sheet->parts.size()).arg(sheet->faces.size())
                    .arg(sheet->sourceDxf ? QStringLiteral("기준 전개 DXF") : QStringLiteral("모델 자동 생성")));
            QGraphicsScene drawingScene;
            fillVerifiedUnrollScene(&drawingScene, *sheet, engine_, batch_);
            if (!drawingScene.items().isEmpty())
                drawingScene.render(&painter, QRectF(40, 135, tableLeft-70, page.height()-180),
                                    drawingScene.sceneRect(), Qt::KeepAspectRatio);
            drawQuantityTable(&painter, &pdf, tableArea, numberedDamages(batch_, part, sheet));
            painter.end();
            statusBar()->showMessage(QStringLiteral("전개 PDF 초안 출력: ") + path, 6000);
            return;
        }
        QGraphicsScene drawingScene;
        fillMapSheetScene(&drawingScene, part, engine_, batch_);
        if (!drawingScene.items().isEmpty())
            drawingScene.render(&painter, QRectF(40, 110, tableLeft-70, page.height()-155),
                                drawingScene.sceneRect(), Qt::KeepAspectRatio);
        drawQuantityTable(&painter, &pdf, tableArea, numberedDamages(batch_, part));
        painter.end();
        statusBar()->showMessage(QStringLiteral("PDF 초안 출력: ") + path, 6000);
    }

    QString projectDb_, inspectionId_;
    QString reviewObjectId_;
    QVector<QVector3D> tracePoints_;
    bool capture_ = false;
    InspectionMapEngine engine_;
    ProjectionBatch batch_;
    QLabel *projectLabel_ = nullptr, *drawingTitle_ = nullptr, *summary_ = nullptr;
    QComboBox *dataset_ = nullptr, *mode_ = nullptr;
    QComboBox *reviewPart_ = nullptr, *reviewDrawing_ = nullptr, *reviewFace_ = nullptr, *traceKind_ = nullptr;
    QLabel *reviewInfo_ = nullptr, *traceCount_ = nullptr;
    QListWidget *parts_ = nullptr, *drawings_ = nullptr;
    QTableWidget *items_ = nullptr;
    QGraphicsScene *scene_ = nullptr;
    DrawingView *view_ = nullptr;
};
}

int main(int argc, char *argv[]) {
    QApplication app(argc, argv);
    const QStringList args = app.arguments();
    const int projectIndex = args.indexOf(QStringLiteral("--project"));
    const int inspectionIndex = args.indexOf(QStringLiteral("--inspection"));
    const int captureIndex = args.indexOf(QStringLiteral("--capture-ui"));
    const int exportIndex = args.indexOf(QStringLiteral("--export-part"));
    const int outputIndex = args.indexOf(QStringLiteral("--output"));
    MapWindow window(projectIndex >= 0 && projectIndex + 1 < args.size() ? args[projectIndex + 1] : QString(),
                     inspectionIndex >= 0 && inspectionIndex + 1 < args.size() ? args[inspectionIndex + 1] : QString());
    const int modeIndex = args.indexOf(QStringLiteral("--mode"));
    if (modeIndex >= 0 && modeIndex + 1 < args.size()) window.setMode(args[modeIndex + 1]);
    window.show();
    if (args.contains(QStringLiteral("--run-projection"))) {
        QTimer::singleShot(0, &window, [&window, args, captureIndex, exportIndex, outputIndex] {
            window.runForBatch();
            if (exportIndex >= 0 && exportIndex + 1 < args.size() &&
                outputIndex >= 0 && outputIndex + 1 < args.size())
                window.exportForBatch(args[exportIndex + 1], args[outputIndex + 1]);
            if (captureIndex >= 0 && captureIndex + 1 < args.size()) {
                window.grab().save(args[captureIndex + 1]);
                QApplication::quit();
            } else if (exportIndex >= 0) {
                QApplication::quit();
            }
        });
    }
    else if (captureIndex >= 0 && captureIndex + 1 < args.size()) {
        const QString path = args[captureIndex + 1];
        QTimer::singleShot(800, &window, [&window, path] {
            window.grab().save(path);
            QApplication::quit();
        });
    }
    return app.exec();
}
