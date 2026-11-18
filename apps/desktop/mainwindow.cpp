#include "mainwindow.h"
#include "./ui_mainwindow.h"

#include "core/Version.h"

MainWindow::MainWindow(QWidget *parent)
    : QMainWindow(parent)
    , ui(new Ui::MainWindow)
{
    ui->setupUi(this);
    setWindowTitle(QStringLiteral("Synkro %1").arg(QString::fromLatin1(synkro::versionString())));
}

MainWindow::~MainWindow()
{
    delete ui;
}
