#pragma once
#include <QMainWindow>
#include <QTextEdit>
#include <QLineEdit>
#include <QPushButton>
#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QKeyEvent>
#include <QStringList>

// Called by the window when the user submits a command.
// Provided by the Rust DLL so the command reaches the UE4 console.
// Receives a UTF-16 string and its codeunit count.
using CommandCallback = void(*)(const wchar_t *cmd, int len);

class DevMenuWindow : public QMainWindow {
    Q_OBJECT

public:
    explicit DevMenuWindow(QWidget *parent = nullptr);

    // Register the Rust-side command dispatcher.  Call once after construction.
    void setCommandCallback(CommandCallback cb);

public slots:
    void submitCommand();
    void appendOutput(const QString &line);

protected:
    bool eventFilter(QObject *obj, QEvent *event) override;

private:
    QTextEdit      *m_output;
    QLineEdit      *m_input;
    QPushButton    *m_submit;

    CommandCallback m_commandCb  = nullptr;
    QStringList     m_history;
    int             m_historyIdx = 0;
};
