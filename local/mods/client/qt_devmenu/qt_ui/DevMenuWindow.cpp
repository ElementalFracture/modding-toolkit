#include "DevMenuWindow.h"
#include <QKeySequence>
#include <QShortcut>
#include <QScrollBar>

static const char *k_stylesheet = R"(
    QMainWindow, QWidget {
        background-color: #111111;
        color: #cccccc;
    }
    QTextEdit {
        background-color: #0a0a0a;
        color: #cccccc;
        border: 1px solid #2a2a2a;
        font-family: Consolas, "Courier New", monospace;
        font-size: 10pt;
        selection-background-color: #1e3a5f;
    }
    QLineEdit {
        background-color: #1a1a1a;
        color: #ffffff;
        border: 1px solid #3a3a3a;
        border-radius: 2px;
        padding: 3px 6px;
        font-family: Consolas, "Courier New", monospace;
        font-size: 10pt;
        selection-background-color: #1e3a5f;
    }
    QLineEdit:focus { border-color: #4a7fbf; }
    QPushButton {
        background-color: #222222;
        color: #aaaaaa;
        border: 1px solid #3a3a3a;
        border-radius: 2px;
        padding: 4px 12px;
    }
    QPushButton:hover  { background-color: #2e2e2e; color: #dddddd; }
    QPushButton:pressed { background-color: #181818; }
    QLabel { color: #666666; font-size: 9pt; }
    QScrollBar:vertical {
        background: #111111;
        width: 8px;
    }
    QScrollBar::handle:vertical {
        background: #333333;
        border-radius: 4px;
        min-height: 20px;
    }
    QScrollBar::add-line:vertical, QScrollBar::sub-line:vertical { height: 0; }
)";

DevMenuWindow::DevMenuWindow(QWidget *parent)
    : QMainWindow(parent)
{
    setWindowTitle("Spellbreak — Developer Console");
    setMinimumSize(820, 520);
    setStyleSheet(k_stylesheet);

    // Qt::Tool keeps it off the taskbar; WindowStaysOnTopHint keeps it over the game.
    setWindowFlags(Qt::Tool | Qt::WindowStaysOnTopHint);

    auto *central = new QWidget(this);
    setCentralWidget(central);

    auto *vbox = new QVBoxLayout(central);
    vbox->setContentsMargins(8, 6, 8, 8);
    vbox->setSpacing(6);

    auto *header = new QLabel("Developer Console  |  Esc or ~ to close", central);
    vbox->addWidget(header);

    m_output = new QTextEdit(central);
    m_output->setReadOnly(true);
    vbox->addWidget(m_output, 1);

    auto *hbox = new QHBoxLayout();
    hbox->setSpacing(4);

    m_input = new QLineEdit(central);
    m_input->setPlaceholderText("Enter command…");
    m_input->installEventFilter(this);
    hbox->addWidget(m_input, 1);

    m_submit = new QPushButton("Run", central);
    m_submit->setFixedWidth(52);
    hbox->addWidget(m_submit);

    vbox->addLayout(hbox);

    connect(m_submit, &QPushButton::clicked,  this, &DevMenuWindow::submitCommand);
    connect(m_input,  &QLineEdit::returnPressed, this, &DevMenuWindow::submitCommand);

    auto *esc = new QShortcut(QKeySequence(Qt::Key_Escape), this);
    connect(esc, &QShortcut::activated, this, &QMainWindow::hide);
}

void DevMenuWindow::setCommandCallback(CommandCallback cb)
{
    m_commandCb = cb;
}

void DevMenuWindow::submitCommand()
{
    const QString cmd = m_input->text().trimmed();
    if (cmd.isEmpty())
        return;

    m_input->clear();
    m_history.append(cmd);
    m_historyIdx = m_history.size();

    appendOutput("<span style='color:#888888;'>&gt; " + cmd.toHtmlEscaped() + "</span>");

    if (m_commandCb) {
        const std::wstring wide = cmd.toStdWString();
        m_commandCb(wide.c_str(), static_cast<int>(wide.size()));
    }
}

void DevMenuWindow::appendOutput(const QString &line)
{
    m_output->append(line);
    m_output->verticalScrollBar()->setValue(
        m_output->verticalScrollBar()->maximum());
}

bool DevMenuWindow::eventFilter(QObject *obj, QEvent *event)
{
    if (obj == m_input && event->type() == QEvent::KeyPress) {
        auto *ke = static_cast<QKeyEvent *>(event);
        if (ke->key() == Qt::Key_Up) {
            if (!m_history.isEmpty() && m_historyIdx > 0) {
                m_historyIdx--;
                m_input->setText(m_history.at(m_historyIdx));
            }
            return true;
        }
        if (ke->key() == Qt::Key_Down) {
            if (m_historyIdx < m_history.size() - 1) {
                m_historyIdx++;
                m_input->setText(m_history.at(m_historyIdx));
            } else {
                m_historyIdx = m_history.size();
                m_input->clear();
            }
            return true;
        }
    }
    return QMainWindow::eventFilter(obj, event);
}
