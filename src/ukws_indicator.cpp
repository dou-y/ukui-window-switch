#include "ukws_indicator.h"
#include "ukws_common.h"
#include "ukws_worker.h"

#include <QDebug>
#include <QScrollArea>
#include <QtX11Extras/QX11Info>
#include <QDesktopWidget>
#include <QApplication>
#include <QStyleOption>
#include <QPainter>
#include <QScrollBar>
#include <QTime>
#include <QCursor>

extern "C" {
#include <X11/X.h>
#include <X11/Xlib.h>
#include <unistd.h>
}

UkwsIndicator::UkwsIndicator(QWidget *parent) : QWidget(parent)
{
    showMode = UkwsIndicator::ShowModeSwitch;
    dragWindow = false;
    showStatus = UkwsWidgetShowStatus::Hidden;
    selIndex = -1;
    cpus = 1;

    wmOperator = new UkwsWnckOperator;
    flowScrollArea = new QScrollArea();
    flowArea = new QWidget();
    winboxFlowLayout = new UkwsFlowLayout(flowArea, 0, 0, 0);

    this->setContentsMargins(0, 0, 0, 0);
    flowArea->setLayout(winboxFlowLayout);
    flowArea->setContentsMargins(0, 0, 0, 0);

    winboxFlowLayout->setAlignment(Qt::AlignCenter);
    winboxFlowLayout->setContentsMargins(0, 0, 0, 0);
    flowScrollArea->setWidgetResizable(true);
    flowScrollArea->setWidget(flowArea);
    flowScrollArea->setContentsMargins(0, 0, 0, 0);

    // 主布局，包含“窗口flow控件”和“已选窗口标题”控件
    mainLayout = new QVBoxLayout();
    mainLayout->addWidget(flowScrollArea);
    mainLayout->setAlignment(Qt::AlignHCenter);
    mainLayout->setMargin(0);

    this->setLayout(mainLayout);

    this->setAttribute(Qt::WA_TranslucentBackground, true);
    this->setObjectName(UKWS_OBJ_IND_SUBWIDGET);
    flowArea->setObjectName(UKWS_OBJ_IND_SUBWIDGET);
    flowScrollArea->setObjectName(UKWS_OBJ_IND_SUBWIDGET);
    this->setObjectName(UKWS_OBJ_IND_MAINWIDGET);

    this->installEventFilter(this);
}

void UkwsIndicator::paintEvent(QPaintEvent *event)
{
    Q_UNUSED(event);
    QStyleOption styleOpt;
    styleOpt.init(this);
    QPainter painter(this);
    style()->drawPrimitive(QStyle::PE_Widget, &styleOpt, &painter, this);

    QWidget::paintEvent(event);
}

void UkwsIndicator::selectWindow(int index)
{
    int size = winboxList.size();

    // 列表为空，无可选择的窗口，直接返回
    if (size <= 0)
        return;

    // 索引大于窗口数量，则代表循环了多轮，需要取模
    if (index >= size)
        index = index % size;

    // 索引小于0，代表逆向循环了多轮
    if (index < 0) {
        while (index < 0) {
            index += size;
        }
    }

    // selIndex超出范围，则还原为默认值
    if ((selIndex < 0) || (selIndex >= size)) {
        selIndex = 0;
    }

    UkwsWindowBox *wb = winboxList.at(selIndex);
    wb->setWindowBoxUnselected();

    selIndex = index;
    wb = winboxList.at(selIndex);
    wb->setWindowBoxSelected();
}

void UkwsIndicator::selectPrevWindow()
{
    int size = winboxList.size();
    UkwsWindowBox *wb = winboxList.at(selIndex);
    wb->setWindowBoxUnselected();

    selIndex--;
    if (selIndex < 0)
        selIndex = size - 1;

    wb = winboxList.at(selIndex);
    wb->setWindowBoxSelected();
    flowScrollArea->ensureWidgetVisible(wb);
}

void UkwsIndicator::selectNextWindow()
{
    int size = winboxList.size();
    UkwsWindowBox *wb = winboxList.at(selIndex);
    wb->setWindowBoxUnselected();

    selIndex++;
    if (selIndex >= size)
        selIndex = 0;

    wb = winboxList.at(selIndex);
    wb->setWindowBoxSelected();
    flowScrollArea->ensureWidgetVisible(wb);
}

void UkwsIndicator::addWinBox(UkwsWindowBox *winbox)
{
    winboxFlowLayout->addWidget(winbox);
    connect(winbox, &UkwsWindowBox::clicked, this, &UkwsIndicator::clickWinbox);
}

void UkwsIndicator::cleanWinBox(int index)
{
    UkwsWindowBox *winbox = winboxList.at(index);
    winboxList.removeAt(index);
    winbox->deleteLater();

}

void UkwsIndicator::cleanAllWinBox()
{
    qDebug() << "UkwsIndicator cleanAllWinBox start";
    // 等待处理完成，并在等待时处理其他事件
    UkwsWorker *worker;
//    for (int i = 0; i < cpus; i++) {
    foreach(worker, workerList) {
        qDebug() << "UkwsIndicator workerList:" << workerList.size() << cpus;
        worker->stopWork();

        while (!worker->doingThread->isFinished()) {
            QCoreApplication::processEvents();
        }
        // 停止所有更新缩略图的线程
        worker->doingThread->quit();

        // doingThread只是保存索引，无父子关系，故需要手动释放
        worker->doingThread->deleteLater();
        worker->deleteLater();
        qDebug() << "UkwsIndicator workerList:";
    }
    workerList.clear();

    UkwsWindowBox *winbox;
    foreach(winbox, winboxList) {
        winboxFlowLayout->removeWidget(winbox);
        winboxList.removeOne(winbox);
        winbox->deleteLater();
    }
    winboxList.clear();
    qDebug() << "UkwsIndicator cleanAllWinBox done";
}

void UkwsIndicator::reloadWindowList(int boxMinHeight)
{
    if (winboxList.size() != 0)
        cleanAllWinBox();

    wmOperator->updateWindowList();
    int size = wmOperator->windowQList->size();

    /*
     * 计算indicator大小、window box大小
     * switch模式：
     *   1. indicator宽高都限定为3/4屏，即上下左右各留3/8的空白区间
     *   2. winbox与indicator间距22px（来自windows）
     *   3. 不需要在indicator中显示当前选定的窗口名称
     *
     * 平铺模式：
     *     对高度进行确定，两行高度计算是否能满足，不行则用三行高度来算
     */

    for (int i = 0; i < size; i++) {
        WnckWindow *win = wmOperator->windowQList->at(i);
        UkwsWindowBox *wb = new UkwsWindowBox;

        wb->index = i;
        wb->setWnckWindow(win);

        // 设置Winbox大小
        wb->setOrigThumbnailByWnck();
        wb->setWinboxSizeByHeight(boxMinHeight);

        winboxList.append(wb);
        wb->setIconByWnck();
        wb->setTitle(wnck_window_get_name(win));

        addWinBox(wb);
    }
}

void UkwsIndicator::reSetWindowThumbnailByWnck()
{
    QThread *workThread;
    UkwsWorker *worker;
    UkwsWindowBox *wb;
    int i;

    // 获取CPU核数
    cpus = sysconf(_SC_NPROCESSORS_ONLN);
    qDebug() << "Get online cpus:" << cpus;
    if (cpus <= 0)
        cpus = 1;

    // 准备工作线程
    for (i = 0; i < cpus; i++) {
        workThread = new QThread;
        worker = new UkwsWorker;

        // doingThread只是保存索引，无父子关系
        worker->doingThread = workThread;
        worker->cpu = i;
        worker->moveToThread(workThread);
        connect(workThread, &QThread::started, worker, &UkwsWorker::doWork);
        connect(worker, &UkwsWorker::workDone, workThread, &QThread::quit);
//        connect(workThread, &QThread::finished, worker, &UkwsWorker::deleteLater);
        workerList.append(worker);
    }

    // 分配工作线程
    int size = winboxList.size();
    for (i = 0; i < size; i++) {
        wb = winboxList.at(i);
        workerList[i % cpus]->workList.append(wb);
    }

    // 开始处理
    for (i = 0; i < cpus; i++) {
        workerList.at(i)->doingThread->start();
    }

    // 等待处理完成，并在等待时处理其他事件
//    for (i = 0; i < cpus; i++) {
//        while (!workerList.at(i)->doingThread->isFinished()) {
//            if (hasStopSignal) {
//                // 收到中断请求，中断各个worker
//                for (int j = 0; j < cpus; j++)
//                    workerList.at(j)->stopWork();
//                qDebug() << "Interrupted" << "11";
//            }

//            QCoreApplication::processEvents();
//        }

//        // 从while中退出，代表工作已完成（无论是否被终止），可以释放资源
//        // doingThread只是保存索引，无父子关系，故需要手动释放
//        workerList.at(i)->doingThread->deleteLater();
//        workerList.at(i)->deleteLater();
//    }

    qDebug() << "reSetWindowThumbnailByWnck done";
}

void UkwsIndicator::reShow(UkwsIndicatorShowMode mode, int minScale)
{
    QSize minSize;
    int maxWidth, maxHeight, winBoxHeight;
    float scale = minScale;
    float actualScale = scale;
    float ratio;

    if (mode != UkwsIndicatorShowMode::ShowModeUnknown)
        this->showMode = mode;
    else if (this->showMode == UkwsIndicatorShowMode::ShowModeUnknown)
        this->showMode = UkwsIndicatorShowMode::ShowModeSwitch;

    if (showStatus != UkwsWidgetShowStatus::Hidden) {
        return;
    }

    showStatus = UkwsWidgetShowStatus::Constructing;

    QDesktopWidget *desktop = QApplication::desktop();

    // 自当前屏幕上显示indicator
    // 获取鼠标所在屏幕
    QRect screenRect;
    int screenIndex = -1;
    for (int i = 0; i < desktop->screenCount(); i++) {
        screenRect = desktop->screenGeometry(i);
        if (screenRect.contains(QCursor::pos()))
            screenIndex = i;
    }

    screenRect =  desktop->availableGeometry(screenIndex);

    if (showMode == UkwsIndicatorShowMode::ShowModeSwitch) {
        maxWidth = screenRect.width() * 4 / 5;
        maxHeight = screenRect.height() * 4 / 5;
    } else {
        maxWidth = screenRect.width();
        maxHeight = screenRect.height();
        maxWidth = maxWidth * 5 / 6 - maxWidth * 1 / 20;
        maxHeight = maxHeight;
    }
    winBoxHeight = ((maxHeight - 5 - 5 - 5 - 5) / scale) - 0 - 32 - 5 - 5;

    // 设置外边距
    mainLayout->setMargin(20);

    // 默认以(maxWidth, maxHeight)为大小，以(maxHeight / 5)为winBox高度
    this->resize(maxWidth, maxHeight);
    this->reloadWindowList(winBoxHeight);

    if (hasStopSignal) {
        showStatus = UkwsWidgetShowStatus::Interrupted;
        cleanStopSignal();
        qDebug() << "Interrupted" << "1";
        return;
    }

    // 无可显示的窗口，直接返回
    if (winboxList.size() <= 0) {
        showStatus = UkwsWidgetShowStatus::Shown;
        return;
    }

    for (int i = (minScale * 2 - 1); i > 2; i--) {
        scale = i / 2.0;
        winBoxHeight = ((maxHeight - 5 - 5 - 5 - 5) / scale) - 0 - 32 - 5 - 5;

        foreach (UkwsWindowBox *wb, winboxList) {
            wb->setWinboxSizeByHeight(winBoxHeight);
        }

        minSize = this->getMaxRect(QRect(0, 0, maxWidth, maxHeight));
        ratio = (float)minSize.width() / minSize.height();

        if ((minSize.height() < maxHeight) && (ratio >= 4 / 3)) {
            actualScale = scale;
        } else
            break;
    }

    winBoxHeight = ((maxHeight - 5 - 5 - 5 - 5) / actualScale) - 0 - 32 - 5 - 5;
    foreach (UkwsWindowBox *wb, winboxList) {
        wb->setWinboxSizeByHeight(winBoxHeight);
    }
    minSize = this->getMaxRect(QRect(0, 0, maxWidth, maxHeight));

    if (showMode == UkwsIndicatorShowMode::ShowModeSwitch) {
        winboxFlowLayout->setSizeConstraint(QLayout::SetMinimumSize);
        if (minSize.width() + 20 * 2 + 4 < maxWidth)
            maxWidth = minSize.width() + 20 * 2 + 4;
        if (minSize.height() + 20 * 2 + 14 < maxHeight)
            maxHeight =  minSize.height() + 20 * 2 + 14;
        this->setFixedSize(maxWidth, maxHeight);
        this->move((screenRect.width() - maxWidth) / 2,
                   (screenRect.height() - maxHeight) / 2);
    }
    this->flowReLayout();
    this->flowScrollArea->verticalScrollBar()->setValue(0);

    this->reSetWindowThumbnailByWnck();
    if (hasStopSignal) {
        showStatus = UkwsWidgetShowStatus::Interrupted;
        cleanStopSignal();
        qDebug() << "Interrupted" << "3";
        return;
    }

    this->show();
    this->activateWindow();

    if (showMode == UkwsIndicatorShowMode::ShowModeSwitch) {
        if (selIndex == -1)
            selIndex = 0;
        selectWindow(selIndex);
    }
    showStatus = UkwsWidgetShowStatus::Shown;
}

void UkwsIndicator::reHide(bool needActivate)
{
    qDebug() << "rehide 1" << showStatus << needActivate;
    if ((showStatus != UkwsWidgetShowStatus::Shown) &&
            (showStatus != UkwsWidgetShowStatus::Interrupted)) {
        return;
    }

    if (winboxList.size() <= 0) {
        showStatus = UkwsWidgetShowStatus::Hidden;
        return;
    }

    showStatus = UkwsWidgetShowStatus::Destructing;
    qDebug() << "rehide 2" << showStatus << needActivate;
    this->hide();
    // 优先处理hide事件，完成后再清理后续
    QCoreApplication::processEvents();
    qDebug() << "test" << selIndex << winboxList.size();

    if (needActivate) {
        UkwsWindowBox *wb = winboxList.at(selIndex);
        qDebug() << "selIndex:" << selIndex << wb->getTitle();
        wb->activateWnckWindow();
        selIndex = -1;
    }
    qDebug() << "rehide 3" << showStatus;

    cleanAllWinBox();

    showStatus = UkwsWidgetShowStatus::Hidden;
}

bool UkwsIndicator::stopConstructing(int timeoutMS)
{
//    qDebug() << "stopConstructing" << showStatus;
    if (showStatus != UkwsWidgetShowStatus::Constructing)
        return true;

    hasStopSignal = true;

    QTime curTime = QTime::currentTime();
    curTime.start();

    while (curTime.elapsed() < timeoutMS) {
        if (showStatus != UkwsWidgetShowStatus::Constructing)
            return true;

        // 每5ms检测一次状态
        usleep(5 * 1000);
//        qDebug() << "waitting stopConstructing";
    }

    return false;
}

void UkwsIndicator::cleanStopSignal()
{
    hasStopSignal = false;
}

void UkwsIndicator::acitveSelectedWindow()
{
    if (winboxList.size() <= 0)
        return;

    UkwsWindowBox *wb = winboxList.at(selIndex);
    wb->activateWnckWindow();
}

void UkwsIndicator::clickWinbox(UkwsWindowBox *wb)
{
    selIndex = winboxList.indexOf(wb);
    qDebug() << "clickWinbox";
    emit isSelected(true);
}

void UkwsIndicator::flowReLayout()
{
    QRect rect = winboxFlowLayout->geometry();
    winboxFlowLayout->actualHeight = rect.height();
    winboxFlowLayout->setGeometry(rect);
}

void UkwsIndicator::moveWindow()
{
    Display *display = QX11Info::display();
    Atom netMoveResize = XInternAtom(display, "_NET_WM_MOVERESIZE", False);
    XEvent xEvent;
    const auto pos = QCursor::pos();

    memset(&xEvent, 0, sizeof(XEvent));
    xEvent.xclient.type = ClientMessage;
    xEvent.xclient.message_type = netMoveResize;
    xEvent.xclient.display = display;
    xEvent.xclient.window = this->winId();
    xEvent.xclient.format = 32;
    xEvent.xclient.data.l[0] = pos.x();
    xEvent.xclient.data.l[1] = pos.y();
    xEvent.xclient.data.l[2] = 8;
    xEvent.xclient.data.l[3] = Button1;
    xEvent.xclient.data.l[4] = 0;

    XUngrabPointer(display, CurrentTime);
    XSendEvent(display, QX11Info::appRootWindow(QX11Info::appScreen()),
               False, SubstructureNotifyMask | SubstructureRedirectMask,
               &xEvent);
    XFlush(display);
}

QSize UkwsIndicator::getMaxRect(const QRect rect, int spaceX, int spaceY)
{
    int maxWidth, maxHight;
    int left, top, right, bottom;
    getContentsMargins(&left, &top, &right, &bottom);
    QRect effectiveRect = rect.adjusted(+left, +top, -right, -bottom);
    int x = effectiveRect.x();
    int y = effectiveRect.y();
    int lineHeight = 0;

    maxWidth = 0;
    maxHight = 0;
    UkwsWindowBox *wb;
    foreach (wb, winboxList) {
        if (spaceX == -1)
            spaceX = wb->style()->layoutSpacing(
                QSizePolicy::PushButton, QSizePolicy::PushButton, Qt::Horizontal);
        if (spaceY == -1)
            spaceY = wb->style()->layoutSpacing(
                QSizePolicy::PushButton, QSizePolicy::PushButton, Qt::Vertical);

        int nextX = x + wb->sizeHint().width() + spaceX;
        if (nextX - spaceX > effectiveRect.right() && lineHeight > 0) {
            x = effectiveRect.x();
            y = y + lineHeight + spaceY;
            nextX = x + wb->sizeHint().width() + spaceX;
            lineHeight = 0;
        }

        if ((x + wb->sizeHint().width()) > maxWidth)
            maxWidth = x + wb->sizeHint().width();

        if ((y + wb->sizeHint().height()) > maxHight)
            maxHight = y + wb->sizeHint().height();

        x = nextX;
        lineHeight = qMax(lineHeight, wb->sizeHint().height());
    }

    return QSize(maxWidth, maxHight);;
}

bool UkwsIndicator::eventFilter(QObject *object, QEvent *event)
{
    if (object == this) {
        if (event->type() == QEvent::MouseButtonPress) {
            return true;
        }

        if (event->type() == QEvent::MouseButtonRelease) {
            QMouseEvent *mouseEvent = static_cast<QMouseEvent *>(event);
            if (mouseEvent->button() == Qt::LeftButton)
                dragWindow = false;
            return true;
        }

        if (event->type() == QEvent::MouseMove) {
            return true;
        }

        if (event->type() == QEvent::WindowDeactivate) {
            if ((showMode == UkwsIndicator::ShowModeSwitch) &&
                    (showStatus == UkwsWidgetShowStatus::Shown ||
                     showStatus == UkwsWidgetShowStatus::Constructing)) {
                qDebug() << "WindowDeactivate";
                // 窗口deactivate，取消任何窗口选择，激活当前窗口
                emit isSelected(false);

                return true;
            }
        }

        if (event->type() == QEvent::Wheel) {
            QWheelEvent *wheelEvent = static_cast<QWheelEvent *>(event);
            QScrollBar *bar = flowScrollArea->verticalScrollBar();
            bar->setValue(bar->value() - wheelEvent->delta());

            return true;
        }

        return false;
    }

    return false;
}
