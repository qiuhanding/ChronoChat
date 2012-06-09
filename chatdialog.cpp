#include <QtGui>
#include "chatdialog.h"
#include "settingdialog.h"
#include <ctime>
#include <iostream>
#include <QTimer>
#include <QMetaType>
#include <QMessageBox>

#define BROADCAST_PREFIX_FOR_SYNC_DEMO "/ndn/broadcast/sync-demo"

void
ChatDialog::testDraw()
{
  std::string prefix[5] = {"/ndn/1", "/ndn/2", "/ndn/3", "/ndn/4", "/ndn/5"};
  std::string nick[5] = {"tom", "jerry", "jason", "michael", "hurry"};
  std::vector<Sync::MissingDataInfo> v;
  for (int i = 0; i < 5; i++)
  {
    Sync::MissingDataInfo mdi = {prefix[i], Sync::SeqNo(0), Sync::SeqNo(i * (2 << i) )};
    v.push_back(mdi);
  }

  m_scene->processUpdate(v, "12341234@!#%!@");

  for (int i = 0; i < 5; i++)
  {
   m_scene-> msgReceived(prefix[i].c_str(), nick[i].c_str());
  }

  fitView();
}

ChatDialog::ChatDialog(QWidget *parent)
  : QDialog(parent), m_sock(NULL)
{
  // have to register this, otherwise
  // the signal-slot system won't recognize this type
  qRegisterMetaType<std::vector<Sync::MissingDataInfo> >("std::vector<Sync::MissingDataInfo>");
  qRegisterMetaType<size_t>("size_t");
  setupUi(this);
  m_session = time(NULL);

  readSettings();

  updateLabels();

  lineEdit->setFocusPolicy(Qt::StrongFocus);
  m_scene = new DigestTreeScene(this);

  treeViewer->setScene(m_scene);
  m_scene->plot("Empty");
  QRectF rect = m_scene->itemsBoundingRect();
  m_scene->setSceneRect(rect);

  // create sync socket
  if(!m_user.getChatroom().isEmpty()) {
    std::string syncPrefix = BROADCAST_PREFIX_FOR_SYNC_DEMO;
    syncPrefix += "/";
    syncPrefix += m_user.getChatroom().toStdString();
    try 
    {
      m_sock = new Sync::SyncAppSocket(syncPrefix, bind(&ChatDialog::processTreeUpdateWrapper, this, _1, _2), bind(&ChatDialog::processRemove, this, _1));
    }
    catch (Sync::CcnxOperationException ex)
    {
      QMessageBox::critical(this, tr("Sync-Demo"), tr("Canno connect to ccnd.\n Have you started your ccnd?"), QMessageBox::Ok);
      std::exit(1);
    }
  }
  
  createActions();
  createTrayIcon();
  connect(lineEdit, SIGNAL(returnPressed()), this, SLOT(returnPressed()));
  connect(setButton, SIGNAL(pressed()), this, SLOT(buttonPressed()));
  connect(this, SIGNAL(dataReceived(QString, const char *, size_t)), this, SLOT(processData(QString, const char *, size_t)));
  connect(this, SIGNAL(treeUpdated(const std::vector<Sync::MissingDataInfo>)), this, SLOT(processTreeUpdate(const std::vector<Sync::MissingDataInfo>)));
  connect(trayIcon, SIGNAL(messageClicked()), this, SLOT(showNormal()));
  connect(trayIcon, SIGNAL(activated(QSystemTrayIcon::ActivationReason)), this, SLOT(iconActivated(QSystemTrayIcon::ActivationReason)));

//testDraw();
}

ChatDialog::~ChatDialog()
{
  if (m_sock != NULL) 
  {
    delete m_sock;
    m_sock = NULL;
  }
}

void 
ChatDialog::setVisible(bool visible)
{
  minimizeAction->setEnabled(visible);
  maximizeAction->setEnabled(!isMaximized());
  restoreAction->setEnabled(isMaximized() || !visible);
  QDialog::setVisible(visible);
}

void 
ChatDialog::closeEvent(QCloseEvent *e)
{
  if (trayIcon->isVisible())
  {
    QMessageBox::information(this, tr("Sync-Demo"),
			     tr("The program will keep running in the "
				"system tray. To terminate the program"
				"choose <b>Quit</b> in the context memu"
				"of the system tray entry."));
    hide();
    e->ignore();
  }
}

void 
ChatDialog::changeEvent(QEvent *e)
{
  switch(e->type()) 
  {
  case QEvent::ActivationChange:
    if (isActiveWindow()) 
    {
      trayIcon->setIcon(QIcon(":/images/icon_small.png"));
    }
    break;
  default:
    break;
  }
}

void
ChatDialog::appendMessage(const SyncDemo::ChatMessage msg) 
{
  boost::mutex::scoped_lock lock(m_msgMutex);

  if (msg.type() != SyncDemo::ChatMessage::CHAT) {
    return;
  }

  if (!msg.has_data()) {
    return;
  }

  if (msg.from().empty() || msg.data().empty()) {
    return;
  }

  QTextCursor cursor(textEdit->textCursor());
  cursor.movePosition(QTextCursor::End);
  QTextTableFormat tableFormat;
  tableFormat.setBorder(0);
  QTextTable *table = cursor.insertTable(1, 2, tableFormat);
  QString from = QString("<%1>: ").arg(msg.from().c_str());
  table->cellAt(0, 0).firstCursorPosition().insertText(from);
  table->cellAt(0, 1).firstCursorPosition().insertText(msg.data().c_str());
  QScrollBar *bar = textEdit->verticalScrollBar();
  bar->setValue(bar->maximum());
  showMessage(from, msg.data().c_str());
}

void
ChatDialog::processTreeUpdateWrapper(const std::vector<Sync::MissingDataInfo> v, Sync::SyncAppSocket *sock)
{
  emit treeUpdated(v);
#ifdef __DEBUG
  std::cout << "<<< Tree update signal emitted" << std::endl;
#endif
}

void
ChatDialog::processTreeUpdate(const std::vector<Sync::MissingDataInfo> v)
{
#ifdef __DEBUG
  std::cout << "<<< processing Tree Update" << std::endl;
#endif
  if (v.empty())
  {
    return;
  }

  // reflect the changes on digest tree
  {
    boost::mutex::scoped_lock lock(m_sceneMutex);
    m_scene->processUpdate(v, m_sock->getRootDigest().c_str());
  }

  int n = v.size();
  int totalMissingPackets = 0;
  for (int i = 0; i < n; i++) 
  {
    totalMissingPackets += v[i].high.getSeq() - v[i].low.getSeq() + 1;
  }
  
  if (totalMissingPackets < 10) {
    for (int i = 0; i < n; i++) 
    {
      for (Sync::SeqNo seq = v[i].low; seq <= v[i].high; ++seq)
      {
        m_sock->fetchRaw(v[i].prefix, seq, bind(&ChatDialog::processDataWrapper, this, _1, _2, _3), 2);
#ifdef __DEBUG
        std::cout << "<<< Fetching " << v[i].prefix << "/" <<seq.getSession() <<"/" << seq.getSeq() << std::endl;
#endif
      }
    }
  }
  else
  {
    // too bad; too many missing packets
    // we may just join a new chatroom
    // or some network patition just healed
    // we don't try to fetch any data in this case (for now)
  }

  // adjust the view
  fitView();

}

void
ChatDialog::processDataWrapper(std::string name, const char *buf, size_t len)
{
  emit dataReceived(name.c_str(), buf, len);
#ifdef __DEBUG
  std::cout <<"<<< " << name << " fetched" << std::endl;
#endif
}

void
ChatDialog::processData(QString name, const char *buf, size_t len)
{
  SyncDemo::ChatMessage msg;
  if (!msg.ParseFromArray(buf, len)) 
  {
    std::cerr << "Errrrr.. Can not parse msg at "<<__FILE__ <<":"<<__LINE__<<". what is happening?" << std::endl;
    abort();
  }

  // display msg received from network
  // we have to do so; this function is called by ccnd thread
  // so if we call appendMsg directly
  // Qt crash as "QObject: Cannot create children for a parent that is in a different thread"
  // the "cannonical" way to is use signal-slot
  appendMessage(msg);
  
  // update the tree view
  std::string stdStrName = name.toStdString();
  std::string stdStrNameWithoutSeq = stdStrName.substr(0, stdStrName.find_last_of('/'));
  std::string prefix = stdStrNameWithoutSeq.substr(0, stdStrNameWithoutSeq.find_last_of('/'));
#ifdef __DEBUG
  std::cout <<"<<< updating scene for" << prefix << ": " << msg.from()  << std::endl;
#endif
  {
    boost::mutex::scoped_lock lock(m_sceneMutex);
    m_scene->msgReceived(prefix.c_str(), msg.from().c_str());
  }
  fitView();
}

void
ChatDialog::processRemove(std::string prefix)
{
}

void
ChatDialog::formChatMessage(const QString &text, SyncDemo::ChatMessage &msg) {
  msg.set_from(m_user.getNick().toStdString());
  msg.set_to(m_user.getChatroom().toStdString());
  msg.set_data(text.toStdString());
  time_t seconds = time(NULL);
  msg.set_timestamp(seconds);
  msg.set_type(SyncDemo::ChatMessage::CHAT);
}

bool
ChatDialog::readSettings()
{
#ifndef __DEBUG
  QSettings s(ORGANIZATION, APPLICATION);
  QString nick = s.value("nick", "").toString();
  QString chatroom = s.value("chatroom", "").toString();
  QString prefix = s.value("prefix", "").toString();
  if (nick == "" || chatroom == "" || prefix == "") {
    QTimer::singleShot(500, this, SLOT(buttonPressed()));
    return false;
  }
  else {
    m_user.setNick(nick);
    m_user.setChatroom(chatroom);
    m_user.setPrefix(prefix);
    return true;
  }
#else
  QTimer::singleShot(500, this, SLOT(buttonPressed()));
  return false;
#endif
}

void 
ChatDialog::writeSettings()
{
#ifndef __DEBUG
  QSettings s(ORGANIZATION, APPLICATION);
  s.setValue("nick", m_user.getNick());
  s.setValue("chatroom", m_user.getChatroom());
  s.setValue("prefix", m_user.getPrefix());
#endif
}

void
ChatDialog::updateLabels()
{
  QString settingDisp = QString("<User: %1>, <Chatroom: %2>").arg(m_user.getNick()).arg(m_user.getChatroom());
  infoLabel->setText(settingDisp);
  QString prefixDisp = QString("<Prefix: %1>").arg(m_user.getPrefix());
  prefixLabel->setText(prefixDisp);
}

void 
ChatDialog::returnPressed()
{
  QString text = lineEdit->text();
  if (text.isEmpty())
    return;
 
  lineEdit->clear();

  SyncDemo::ChatMessage msg;
  formChatMessage(text, msg);
  
  appendMessage(msg);

  // send msg
  size_t size = msg.ByteSize();
  char *buf = new char[size];
  msg.SerializeToArray(buf, size);
  if (!msg.IsInitialized()) 
  {
    std::cerr << "Errrrr.. msg was not probally initialized "<<__FILE__ <<":"<<__LINE__<<". what is happening?" << std::endl;
    abort();
  }
  m_sock->publishRaw(m_user.getPrefix().toStdString(), m_session, buf, size, 60);

  int nextSequence = m_sock->getNextSeq(m_user.getPrefix().toStdString(), m_session);
  Sync::MissingDataInfo mdi = {m_user.getPrefix().toStdString(), Sync::SeqNo(0), Sync::SeqNo(nextSequence - 1)};
  std::vector<Sync::MissingDataInfo> v;
  v.push_back(mdi);
  {
    boost::mutex::scoped_lock lock(m_sceneMutex);
    m_scene->processUpdate(v, m_sock->getRootDigest().c_str());
    m_scene->msgReceived(m_user.getPrefix(), m_user.getNick());
  }
  fitView();
}

void
ChatDialog::buttonPressed()
{
  SettingDialog dialog(this, m_user.getNick(), m_user.getChatroom(), m_user.getPrefix());
  connect(&dialog, SIGNAL(updated(QString, QString, QString)), this, SLOT(settingUpdated(QString, QString, QString)));
  dialog.exec();
  QTimer::singleShot(100, this, SLOT(checkSetting()));
}

void
ChatDialog::checkSetting()
{
  if (m_user.getPrefix().isEmpty() || m_user.getNick().isEmpty() || m_user.getChatroom().isEmpty())
  {
    buttonPressed();
  }
}

void
ChatDialog::settingUpdated(QString nick, QString chatroom, QString prefix)
{
  bool needWrite = false;
  if (!nick.isEmpty() && nick != m_user.getNick()) {
    m_user.setNick(nick);
    needWrite = true;
  }
  if (!prefix.isEmpty() && prefix != m_user.getPrefix()) {
    m_user.setPrefix(prefix);
    needWrite = true;
    // TODO: set the previous prefix as left?
  }
  if (!chatroom.isEmpty() && chatroom != m_user.getChatroom()) {
    m_user.setChatroom(chatroom);
    needWrite = true;

    {
      boost::mutex::scoped_lock lock(m_sceneMutex);
      m_scene->clearAll();
      m_scene->plot("Empty");
    }
    // TODO: perhaps need to do a lot. e.g. use a new SyncAppSokcet
    if (m_sock != NULL) 
    {
      delete m_sock;
      m_sock = NULL;
    }
    std::string syncPrefix = BROADCAST_PREFIX_FOR_SYNC_DEMO;
    syncPrefix += "/";
    syncPrefix += m_user.getChatroom().toStdString();
    try
    {
      m_sock = new Sync::SyncAppSocket(syncPrefix, bind(&ChatDialog::processTreeUpdateWrapper, this, _1, _2), bind(&ChatDialog::processRemove, this, _1));
    }
    catch (Sync::CcnxOperationException ex)
    {
      QMessageBox::critical(this, tr("Sync-Demo"), tr("Canno connect to ccnd.\n Have you started your ccnd?"), QMessageBox::Ok);
      std::exit(1);
    }

    fitView();
    
  }
  if (needWrite) {
    writeSettings();
    updateLabels();
  }
}

void
ChatDialog::iconActivated(QSystemTrayIcon::ActivationReason reason)
{
  switch (reason)
  {
  case QSystemTrayIcon::Trigger:
  case QSystemTrayIcon::DoubleClick:
    break;
  case QSystemTrayIcon::MiddleClick:
    // showMessage();
    break;
  default:;
  }
}

void
ChatDialog::showMessage(QString from, QString data)
{
  //  std::cout <<"Showing Message: " << from.toStdString() << ": " << data.toStdString() << std::endl;
  if (!isActiveWindow()) 
  {
    trayIcon->showMessage(QString("Chatroom %1 has a new message").arg(m_user.getChatroom()), QString("<%1>: %2").arg(from).arg(data), QSystemTrayIcon::Information, 20000);
    trayIcon->setIcon(QIcon(":/images/note.png"));
  }
}

void
ChatDialog::messageClicked()
{
  this->showMaximized();
}

void
ChatDialog::createActions()
{
  minimizeAction = new QAction(tr("Mi&nimize"), this);
  connect(minimizeAction, SIGNAL(triggered()), this, SLOT(hide()));
  
  maximizeAction = new QAction(tr("Ma&ximize"), this);
  connect(maximizeAction, SIGNAL(triggered()), this, SLOT(showMaximized()));
  
  restoreAction = new QAction(tr("&Restore"), this);
  connect(restoreAction, SIGNAL(triggered()), this, SLOT(showNormal()));
  
  quitAction = new QAction(tr("Quit"), this);
  connect(quitAction, SIGNAL(triggered()), qApp, SLOT(quit()));
}

void
ChatDialog::createTrayIcon()
{
  trayIconMenu = new QMenu(this);
  trayIconMenu->addAction(minimizeAction);
  trayIconMenu->addAction(maximizeAction);
  trayIconMenu->addAction(restoreAction);
  trayIconMenu->addSeparator();
  trayIconMenu->addAction(quitAction);

  trayIcon = new QSystemTrayIcon(this);
  trayIcon->setContextMenu(trayIconMenu);

  QIcon icon(":/images/icon_small.png");
  trayIcon->setIcon(icon);
  setWindowIcon(icon);
  trayIcon->setToolTip("Sync-Demo System Tray Icon");
  trayIcon->setVisible(true);
}

void 
ChatDialog::resizeEvent(QResizeEvent *e)
{
  fitView();
}

void 
ChatDialog::showEvent(QShowEvent *e)
{
  fitView();
}

void
ChatDialog::fitView()
{
  boost::mutex::scoped_lock lock(m_sceneMutex);
  QRectF rect = m_scene->itemsBoundingRect();
  m_scene->setSceneRect(rect);
  treeViewer->fitInView(m_scene->itemsBoundingRect(), Qt::KeepAspectRatio);
}
