/** This file is part of Contacts daemon
 **
 ** Copyright (c) 2010-2011 Nokia Corporation and/or its subsidiary(-ies).
 **
 ** Contact:  Nokia Corporation (info@qt.nokia.com)
 **
 ** GNU Lesser General Public License Usage
 ** This file may be used under the terms of the GNU Lesser General Public License
 ** version 2.1 as published by the Free Software Foundation and appearing in the
 ** file LICENSE.LGPL included in the packaging of this file.  Please review the
 ** following information to ensure the GNU Lesser General Public License version
 ** 2.1 requirements will be met:
 ** http://www.gnu.org/licenses/old-licenses/lgpl-2.1.html.
 **
 ** In addition, as a special exception, Nokia gives you certain additional rights.
 ** These rights are described in the Nokia Qt LGPL Exception version 1.1, included
 ** in the file LGPL_EXCEPTION.txt in this package.
 **
 ** Other Usage
 ** Alternatively, this file may be used in accordance with the terms and
 ** conditions contained in a signed written agreement between you and Nokia.
 **/

#include <QContact>
#include <QContactFetchByIdRequest>
#include <QContactLocalIdFetchRequest>
#include <QContactFetchRequest>
#include <QContactRemoveRequest>
#include <QContactSaveRequest>
#include <QContactLocalIdFilter>
#include <QContactOnlineAccount>

#include <qtcontacts-tracker/contactmergerequest.h>

#include <TelepathyQt4/Debug>

#include "libtelepathy/util.h"
#include "libtelepathy/debug.h"

#include "test-telepathy-plugin.h"
#include "buddymanagementinterface.h"
#include "debug.h"

TestTelepathyPlugin::TestTelepathyPlugin(QObject *parent) : Test(parent),
        mExpectation(0)
{
}

void TestTelepathyPlugin::initTestCase()
{
    initTestCaseImpl();

    qRegisterMetaType<QContactAbstractRequest::State>("QContactAbstractRequest::State");

    g_type_init();
    g_set_prgname("test-telepathy-plugin");

    if (!qgetenv("CONTACTSD_DEBUG").isEmpty()) {
        tp_debug_set_flags("all");
        test_debug_enable(TRUE);
        enableDebug(true);
    }

    dbus_g_bus_get(DBUS_BUS_STARTER, 0);

    /* Create a QContactManager and track added/changed contacts */
    mContactManager = new QContactManager(QLatin1String("tracker"));
    connect(mContactManager,
            SIGNAL(contactsAdded(const QList<QContactLocalId>&)),
            SLOT(contactsAdded(const QList<QContactLocalId>&)));
    connect(mContactManager,
            SIGNAL(contactsChanged(const QList<QContactLocalId>&)),
            SLOT(contactsChanged(const QList<QContactLocalId>&)));
    connect(mContactManager,
            SIGNAL(contactsRemoved(const QList<QContactLocalId>&)),
            SLOT(contactsRemoved(const QList<QContactLocalId>&)));
    mLocalContactIds += mContactManager->selfContactId();

    /* Create a fake AccountManager */
    TpDBusDaemon *dbus = tp_dbus_daemon_dup(NULL);
    mAccountManager = (TpTestsSimpleAccountManager *) tp_tests_object_new_static_class(
            TP_TESTS_TYPE_SIMPLE_ACCOUNT_MANAGER, NULL);
    tp_dbus_daemon_register_object(dbus, TP_ACCOUNT_MANAGER_OBJECT_PATH, mAccountManager);
    tp_dbus_daemon_request_name (dbus, TP_ACCOUNT_MANAGER_BUS_NAME, FALSE, NULL);
    g_object_unref(dbus);
}

void TestTelepathyPlugin::cleanupTestCase()
{
    cleanupTestCaseImpl();

    delete mContactManager;
    g_object_unref(mAccountManager);
}

void TestTelepathyPlugin::init()
{
    initImpl();

    /* Create a fake Connection */
    tp_tests_create_and_connect_conn(TP_TESTS_TYPE_CONTACTS_CONNECTION,
            "fakeaccount", &mConnService, &mConnection);
    QVERIFY(mConnService);
    QVERIFY(mConnection);

    mListManager = tp_tests_contacts_connection_get_contact_list_manager(
        TP_TESTS_CONTACTS_CONNECTION(mConnService));

    /* Define the self contact */
    TpTestsContactsConnectionPresenceStatusIndex presence =
            TP_TESTS_CONTACTS_CONNECTION_STATUS_AVAILABLE;
    const gchar *message = "Running unit tests";
    tp_tests_contacts_connection_change_presences(
        TP_TESTS_CONTACTS_CONNECTION (mConnService),
        1, &mConnService->self_handle, &presence, &message);
    const gchar *alias = "badger";
    tp_tests_contacts_connection_change_aliases(
        TP_TESTS_CONTACTS_CONNECTION (mConnService),
        1, &mConnService->self_handle, &alias);

    /* Create a fake Account */
    TpDBusDaemon *dbus = tp_dbus_daemon_dup(NULL);
    mAccount = (TpTestsSimpleAccount *) tp_tests_object_new_static_class(
            TP_TESTS_TYPE_SIMPLE_ACCOUNT, NULL);
    tp_dbus_daemon_register_object(dbus, ACCOUNT_PATH, mAccount);
    tp_tests_simple_account_manager_add_account (mAccountManager, ACCOUNT_PATH, TRUE);
    tp_tests_simple_account_set_connection (mAccount, mConnService->object_path);
    g_object_unref(dbus);

    /* Wait for self contact to appear */
    TestExpectationInit exp;
    runExpectation(&exp);
}

void TestTelepathyPlugin::cleanup()
{
    cleanupImpl();

    tp_cli_connection_call_disconnect(mConnection, -1, NULL, NULL, NULL, NULL);
    tp_tests_simple_account_manager_remove_account(mAccountManager, ACCOUNT_PATH);
    tp_tests_simple_account_removed(mAccount);

    /* Wait for all contacts to disappear, and local contacts to get updated */
    TestExpectationCleanup exp(mLocalContactIds.count());
    runExpectation(&exp);

    /* Remove remaining local contacts */
    QList<QContactLocalId> contactsToRemove = mLocalContactIds;
    contactsToRemove.removeOne(mContactManager->selfContactId());
    if (!contactsToRemove.isEmpty()) {
        QContactRemoveRequest *request = new QContactRemoveRequest();
        request->setContactIds(contactsToRemove);
        startRequest(request);

        TestExpectationMass exp(0, 0, contactsToRemove.count());
        runExpectation(&exp);
    }

    QVERIFY(mLocalContactIds.count() == 1);

    g_object_unref(mConnService);
    g_object_unref(mConnection);
    g_object_unref(mAccount);
}

void TestTelepathyPlugin::testBasicUpdates()
{
    /* Create a new contact */
    TpHandle handle = ensureContact("testbasicupdates");

    /* Set alias to "Alice" */
    const char *alias = "Alice";
    tp_tests_contacts_connection_change_aliases(
        TP_TESTS_CONTACTS_CONNECTION (mConnService),
        1, &handle, &alias);

    /* Add it in the ContactList */
    test_contact_list_manager_request_subscription(mListManager, 1, &handle,
        "wait");

    TestExpectationContact exp(EventAdded, "testbasicupdates");
    exp.verifyAlias(alias);
    exp.verifyPresence(TP_TESTS_CONTACTS_CONNECTION_STATUS_UNKNOWN);
    exp.verifyAuthorization("Requested", "No");
    runExpectation(&exp);

    /* Change the presence to busy */
    TpTestsContactsConnectionPresenceStatusIndex presence =
            TP_TESTS_CONTACTS_CONNECTION_STATUS_BUSY;
    const gchar *message = "Making coffee";
    tp_tests_contacts_connection_change_presences(
        TP_TESTS_CONTACTS_CONNECTION (mConnService),
        1, &handle, &presence, &message);

    exp.setEvent(EventChanged);
    exp.verifyPresence(presence);
    runExpectation(&exp);
}

void TestTelepathyPlugin::testSelfContact()
{
    const gchar *alias = "Unit Test";
    tp_tests_contacts_connection_change_aliases(
        TP_TESTS_CONTACTS_CONNECTION (mConnService),
        1, &mConnService->self_handle, &alias);

    TestExpectationContact exp(EventChanged);
    exp.verifyAlias(alias);
    exp.verifyPresence(TP_TESTS_CONTACTS_CONNECTION_STATUS_AVAILABLE);
    exp.verifyAvatar(QByteArray());
    runExpectation(&exp);
}

void TestTelepathyPlugin::testAuthorization()
{
    /* Create a new contact "romeo" */
    TpHandle handle = ensureContact("romeo");

    /* Add him in the ContactList, the request will be ignored */
    test_contact_list_manager_request_subscription(mListManager, 1, &handle,
        "wait");

    TestExpectationContact exp(EventAdded, "romeo");
    exp.verifyAuthorization("Requested", "No");
    runExpectation(&exp);

    /* Ask again for subscription, say "please" this time so it gets accepted */
    test_contact_list_manager_request_subscription(mListManager, 1, &handle,
        "please");

    exp.setEvent(EventChanged);
    exp.verifyAuthorization("Yes", "Requested");
    runExpectation(&exp);

    handle = ensureContact("juliette");

    /* Add her in the ContactList, the request will be ignored */
    test_contact_list_manager_request_subscription(mListManager, 1, &handle,
        "wait");

    TestExpectationContact exp2(EventAdded, "juliette");
    exp2.verifyAuthorization("Requested", "No");
    runExpectation(&exp2);

    /* Ask again for subscription, but this time it will be rejected */
    test_contact_list_manager_request_subscription(mListManager, 1, &handle,
        "no");

    exp2.setEvent(EventChanged);
    exp2.verifyAuthorization("No", "No");
    runExpectation(&exp2);
}

GPtrArray *TestTelepathyPlugin::createContactInfoTel(const gchar *number)
{
    GPtrArray *infoPtrArray;

    infoPtrArray = g_ptr_array_new_with_free_func((GDestroyNotify) g_value_array_free);
    const gchar *fieldValues[] = { number, NULL };
    g_ptr_array_add (infoPtrArray, tp_value_array_build(3,
        G_TYPE_STRING, "tel",
        G_TYPE_STRV, NULL,
        G_TYPE_STRV, fieldValues,
        G_TYPE_INVALID));

    return infoPtrArray;
}

void TestTelepathyPlugin::testContactInfo()
{
    /* Create a contact with no ContactInfo */
    TpHandle handle = ensureContact("testcontactinfo");
    test_contact_list_manager_request_subscription(mListManager, 1, &handle,
        "wait");

    TestExpectationContact exp(EventAdded, "testcontactinfo");
    runExpectation(&exp);

    /* Set some ContactInfo on the contact */
    GPtrArray *infoPtrArray = createContactInfoTel("123");
    {
        const gchar *fieldValues[] = { "email@foo.com", NULL };
        const gchar *fieldParams[] = { "type=work", "type=something totally wrong", "some garbage", NULL };
        g_ptr_array_add (infoPtrArray, tp_value_array_build(3,
            G_TYPE_STRING, "email",
            G_TYPE_STRV, fieldParams,
            G_TYPE_STRV, fieldValues,
            G_TYPE_INVALID));
    }
    tp_tests_contacts_connection_change_contact_info(
        TP_TESTS_CONTACTS_CONNECTION(mConnService), handle, infoPtrArray);

    exp.setEvent(EventChanged);
    exp.verifyInfo(infoPtrArray);
    runExpectation(&exp);
    g_ptr_array_unref(infoPtrArray);

    /* Change the ContactInfo */
    infoPtrArray = createContactInfoTel("456");
    {
        const gchar *fieldValues[] = { "789", NULL };
        const gchar *fieldParams[] = { "type=home", "type=cell", "type=video", NULL };
        g_ptr_array_add (infoPtrArray, tp_value_array_build(3,
            G_TYPE_STRING, "tel",
            G_TYPE_STRV, fieldParams,
            G_TYPE_STRV, fieldValues,
            G_TYPE_INVALID));
    }
    {
        const gchar *fieldValues[] = { "789", NULL };
        const gchar *fieldParams[] = { "type=work", NULL };
        g_ptr_array_add (infoPtrArray, tp_value_array_build(3,
            G_TYPE_STRING, "tel",
            G_TYPE_STRV, fieldParams,
            G_TYPE_STRV, fieldValues,
            G_TYPE_INVALID));
    }
    tp_tests_contacts_connection_change_contact_info(
        TP_TESTS_CONTACTS_CONNECTION(mConnService), handle, infoPtrArray);

    exp.verifyInfo(infoPtrArray);
    runExpectation(&exp);
    g_ptr_array_unref(infoPtrArray);
}

void TestTelepathyPlugin::testContactPhoneNumber()
{
    /* Create a contact with no ContactInfo */
    TpHandle handle = ensureContact("failphone");
    test_contact_list_manager_request_subscription(mListManager, 1, &handle,
        "wait");

    TestExpectationContact exp(EventAdded, "failphone");
    runExpectation(&exp);

    /* Set some ContactInfo on the contact */
    GPtrArray *infoPtrArray = createContactInfoTel("+8888");
    tp_tests_contacts_connection_change_contact_info(
        TP_TESTS_CONTACTS_CONNECTION(mConnService), handle, infoPtrArray);

    exp.setEvent(EventChanged);
    exp.verifyInfo(infoPtrArray);
    runExpectation(&exp);
    g_ptr_array_unref(infoPtrArray);

    /* Change the ContactInfo */
    infoPtrArray = createContactInfoTel("+8887");
    tp_tests_contacts_connection_change_contact_info(
        TP_TESTS_CONTACTS_CONNECTION(mConnService), handle, infoPtrArray);

    exp.verifyInfo(infoPtrArray);
    runExpectation(&exp);
    g_ptr_array_unref(infoPtrArray);

    /* Change the ContactInfo */
    infoPtrArray = createContactInfoTel("+8888");
    tp_tests_contacts_connection_change_contact_info(
        TP_TESTS_CONTACTS_CONNECTION(mConnService), handle, infoPtrArray);

    exp.verifyInfo(infoPtrArray);
    runExpectation(&exp);
    g_ptr_array_unref(infoPtrArray);
}

void TestTelepathyPlugin::testBug220851()
{
    /* Create a contact with no ContactInfo */
    TpHandle handle = ensureContact("testbug220851");
    test_contact_list_manager_request_subscription(mListManager, 1, &handle,
        "wait");

    TestExpectationContact exp(EventAdded, "testbug220851");
    runExpectation(&exp);

    /* An address has 7 fields normally. Verify it's fine to give less */
    GPtrArray *infoPtrArray = g_ptr_array_new_with_free_func((GDestroyNotify) g_value_array_free);
    const gchar *fieldValues[] = { "pobox", "extendedaddress", "street", NULL };
    g_ptr_array_add (infoPtrArray, tp_value_array_build(3,
        G_TYPE_STRING, "adr",
        G_TYPE_STRV, NULL,
        G_TYPE_STRV, fieldValues,
        G_TYPE_INVALID));

    tp_tests_contacts_connection_change_contact_info(
        TP_TESTS_CONTACTS_CONNECTION(mConnService), handle, infoPtrArray);

    exp.setEvent(EventChanged);
    exp.verifyInfo(infoPtrArray);
    runExpectation(&exp);
    g_ptr_array_unref(infoPtrArray);
}

void TestTelepathyPlugin::testRemoveContacts()
{
    TpHandle handle = ensureContact("testremovecontacts");
    test_contact_list_manager_request_subscription(mListManager, 1, &handle,
        "please");

    TestExpectationContact exp(EventAdded, "testremovecontacts");
    runExpectation(&exp);

    test_contact_list_manager_remove(mListManager, 1, &handle);
    exp.setEvent(EventRemoved);
    runExpectation(&exp);
}

void TestTelepathyPlugin::testRemoveBuddyDBusAPI()
{
    // Create 2 contacts
    const char *buddy1 = "removebuddy1";
    TpHandle handle1 = ensureContact(buddy1);
    test_contact_list_manager_request_subscription(mListManager, 1, &handle1, "wait");
    TestExpectationContact exp1(EventAdded, buddy1);
    runExpectation(&exp1);

    const char *buddy2 = "removebuddy2";
    TpHandle handle2 = ensureContact(buddy2);
    test_contact_list_manager_request_subscription(mListManager, 1, &handle2, "wait");
    TestExpectationContact exp2(EventAdded, buddy2);
    runExpectation(&exp2);

    // Remove buddy1 when account is online
    BuddyManagementInterface *buddyIf = new BuddyManagementInterface("com.nokia.contactsd", "/telepathy", QDBusConnection::sessionBus(), 0);
    {
        QDBusPendingReply<> async = buddyIf->removeBuddies(ACCOUNT_PATH, QStringList() << buddy1);
        QDBusPendingCallWatcher watcher(async, this);
        watcher.waitForFinished();
        QVERIFY2(not async.isError(), async.error().message().toLatin1());
        QVERIFY(watcher.isValid());
    }
    exp1.setEvent(EventRemoved);
    runExpectation(&exp1);

    // Set account offline to test offline removal
    tp_cli_connection_call_disconnect(mConnection, -1, NULL, NULL, NULL, NULL);
    TestExpectationDisconnect exp3(mLocalContactIds.count());
    runExpectation(&exp3);

    // Remove buddy2 when account is offline
    {
        QDBusPendingReply<> async = buddyIf->removeBuddies(ACCOUNT_PATH, QStringList() << buddy2);
        QDBusPendingCallWatcher watcher(async, this);
        watcher.waitForFinished();
        QVERIFY2(not async.isError(), async.error().message().toLatin1());
        QVERIFY(watcher.isValid());
    }
    exp2.setEvent(EventRemoved);
    runExpectation(&exp2);

    // FIXME: We should somehow verify that when setting account back online,
    // buddy2 gets removed from roster
}

void TestTelepathyPlugin::testInviteBuddyDBusAPI()
{
    const QString buddy("invitebuddy");
    BuddyManagementInterface *buddyIf = new BuddyManagementInterface("com.nokia.contactsd", "/telepathy", QDBusConnection::sessionBus(), 0);
    {
        QDBusPendingReply<> async = buddyIf->inviteBuddies(ACCOUNT_PATH, QStringList() << buddy);
        QDBusPendingCallWatcher watcher(async, this);
        watcher.waitForFinished();
        QVERIFY2(not async.isError(), async.error().message().toLatin1());
        QVERIFY(watcher.isValid());
    }

    TestExpectationContact exp(EventAdded, buddy);
    runExpectation(&exp);
}

void TestTelepathyPlugin::testSetOffline()
{
    TpHandle handle = ensureContact("testsetoffline");
    test_contact_list_manager_request_subscription(mListManager, 1, &handle,
        "please");

    TestExpectationContact exp(EventAdded, "testsetoffline");
    runExpectation(&exp);

    tp_cli_connection_call_disconnect(mConnection, -1, NULL, NULL, NULL, NULL);

    TestExpectationDisconnect exp2(mLocalContactIds.count());
    runExpectation(&exp2);
}

void TestTelepathyPlugin::testAvatar()
{
    const gchar avatarData[] = "fake-avatar-data";
    const gchar avatarToken[] = "fake-avatar-token";
    const gchar avatarMimeType[] = "fake-avatar-mime-type";

    /* Create a contact with an avatar  */
    TpHandle handle = ensureContact("testavatar");
    GArray *array = g_array_new(FALSE, FALSE, sizeof(gchar));
    g_array_append_vals(array, avatarData, strlen(avatarData));
    tp_tests_contacts_connection_change_avatar_data(
        TP_TESTS_CONTACTS_CONNECTION (mConnService),
        handle, array, avatarMimeType, avatarToken);
    g_array_unref(array);

    test_contact_list_manager_request_subscription(mListManager, 1, &handle,
        "please");

    TestExpectationContact exp(EventAdded, "testavatar");
    exp.verifyAvatar(QByteArray(avatarData));
    runExpectation(&exp);

    /* Change avatar */
    const gchar avatarData2[] = "fake-avatar-data-2";
    const gchar avatarToken2[] = "fake-avatar-token-2";
    const gchar avatarMimeType2[] = "fake-avatar-mime-type-2";
    array = g_array_new(FALSE, FALSE, sizeof(gchar));
    g_array_append_vals(array, avatarData2, strlen(avatarData2));
    tp_tests_contacts_connection_change_avatar_data(
        TP_TESTS_CONTACTS_CONNECTION (mConnService),
        handle, array, avatarMimeType2, avatarToken2);
    g_array_unref(array);

    exp.setEvent(EventChanged);
    exp.verifyAvatar(QByteArray(avatarData2));
    runExpectation(&exp);

    /* Set back initial avatar */
    array = g_array_new(FALSE, FALSE, sizeof(gchar));
    g_array_append_vals(array, avatarData, strlen(avatarData));
    tp_tests_contacts_connection_change_avatar_data(
        TP_TESTS_CONTACTS_CONNECTION (mConnService),
        handle, array, avatarMimeType, avatarToken);
    g_array_unref(array);

    exp.verifyAvatar(QByteArray(avatarData));
    runExpectation(&exp);

    /* Create another contact with the same avatar, they will share the same
     * nfo:FileObjectData in tracker */
    TpHandle handle2 = ensureContact("testavatar2");
    array = g_array_new(FALSE, FALSE, sizeof(gchar));
    g_array_append_vals(array, avatarData, strlen(avatarData));
    tp_tests_contacts_connection_change_avatar_data(
        TP_TESTS_CONTACTS_CONNECTION (mConnService),
        handle2, array, avatarMimeType, avatarToken);
    g_array_unref(array);

    test_contact_list_manager_request_subscription(mListManager, 1, &handle2,
        "please");

    TestExpectationContact exp2(EventAdded, "testavatar2");
    exp2.verifyAvatar(QByteArray(avatarData));
    runExpectation(&exp2);

    /* Change avatar of the new contact */
    array = g_array_new(FALSE, FALSE, sizeof(gchar));
    g_array_append_vals(array, avatarData2, strlen(avatarData2));
    tp_tests_contacts_connection_change_avatar_data(
        TP_TESTS_CONTACTS_CONNECTION (mConnService),
        handle2, array, avatarMimeType2, avatarToken2);
    g_array_unref(array);

    exp2.setEvent(EventChanged);
    exp2.verifyAvatar(QByteArray(avatarData2));
    runExpectation(&exp2);

    /* Change the alias of first contact, this is to force fetching again
     * the contact, so verify the shared nfo:FileObjectData for the avatar is
     * still there */
    const char *alias = "Where is my Avatar ?";
    tp_tests_contacts_connection_change_aliases(
        TP_TESTS_CONTACTS_CONNECTION (mConnService),
        1, &handle, &alias);

    exp.verifyAlias(alias);
    runExpectation(&exp);
}

void TestTelepathyPlugin::testIRIEncode()
{
    /* Create a contact with a special id that could confuse tracker */
    TpHandle handle = ensureContact("<specialid>");
    test_contact_list_manager_request_subscription(mListManager, 1, &handle,
        "wait");

    TestExpectationContact exp(EventAdded);
    runExpectation(&exp);
}

void TestTelepathyPlugin::testBug253679()
{
    const char *id = "testbug253679";

    /* Create a local contact with an OnlineAccount */
    QContact contact;
    QContactOnlineAccount onlineAccount;
    onlineAccount.setAccountUri(id);
    onlineAccount.setValue(QLatin1String("AccountPath"), QLatin1String(ACCOUNT_PATH));
    contact.saveDetail(&onlineAccount);

    QContactSaveRequest *request = new QContactSaveRequest();
    request->setContact(contact);
    startRequest(request);

    TestExpectationContact exp(EventAdded);
    exp.verifyGenerator("addressbook");
    runExpectation(&exp);

    /* Now add the same OnlineAccount in our telepathy account, we expect this
     * to update existing contact and not create a new one */
    TpHandle handle = ensureContact(id);
    test_contact_list_manager_request_subscription(mListManager, 1, &handle,
        "wait");
    const char *alias = "NB#253679";
    tp_tests_contacts_connection_change_aliases(
        TP_TESTS_CONTACTS_CONNECTION (mConnService),
        1, &handle, &alias);

    exp.setEvent(EventChanged);
    exp.verifyAlias(alias);
    runExpectation(&exp);
}

void TestTelepathyPlugin::testMergedContact()
{
    /* create new contact */
    TpHandle handle1 = ensureContact("contact1");
    test_contact_list_manager_request_subscription(mListManager, 1, &handle1,
        "wait");
    TestExpectationContact exp1(EventAdded, "contact1");
    runExpectation(&exp1);

    /* create another contact */
    TpHandle handle2 = ensureContact("contact2");
    test_contact_list_manager_request_subscription(mListManager, 1, &handle2,
        "wait");
    TestExpectationContact exp2(EventAdded, "contact2");
    runExpectation(&exp2);

    /* Merge contact2 into contact1 */
    QContact contact1 = exp1.contact();
    QContact contact2 = exp2.contact();
    const QList<QContactLocalId> mergeIds = QList<QContactLocalId>() << contact2.localId();
    mergeContacts(contact1, mergeIds);
    exp1.verifyLocalId(contact1.localId());
    exp1.verifyGenerator("telepathy");
    exp2.verifyLocalId(contact1.localId());
    exp2.verifyGenerator("telepathy");
    const QList<TestExpectationContact *> expectations = QList<TestExpectationContact *>() << &exp1 << &exp2;
    TestExpectationMerge exp3(contact1.localId(), mergeIds, expectations);
    runExpectation(&exp3);

    /* Change presence of contact1, verify it modify the global presence */
    TpTestsContactsConnectionPresenceStatusIndex presence =
            TP_TESTS_CONTACTS_CONNECTION_STATUS_BUSY;
    const gchar *message = "Testing merged contact";
    tp_tests_contacts_connection_change_presences(
        TP_TESTS_CONTACTS_CONNECTION (mConnService),
        1, &handle1, &presence, &message);
    TestExpectationContact exp4(EventChanged);
    exp4.verifyLocalId(contact1.localId());
    exp4.verifyPresence(presence);
    runExpectation(&exp4);

#if 0
    /* Change alias of contact2, verify it modify the global nickname */
    const char *alias = "Merged contact 2";
    tp_tests_contacts_connection_change_aliases(
        TP_TESTS_CONTACTS_CONNECTION (mConnService),
        1, &handle2, &alias);
    exp4.verifyAlias(alias);
    runExpectation(&exp4);

    /* NB#223264 - Contactsd incorrectly handle the removal of merged IM contacts */
    tp_tests_simple_account_disabled (mAccount, FALSE);

    QContactFetchRequest *request = new QContactFetchRequest();
    request->setManager(mContactManager);
    QContactLocalIdFilter filter;
    filter.setIds(QList<QContactLocalId>() << contact1.localId());
    request->setFilter(filter);
    connect(request, SIGNAL(resultsAvailable()),
        SLOT(onContactsFetched()));
    QVERIFY(request->start());
#endif
}

void TestTelepathyPlugin::mergeContacts(const QContact &contactTarget,
        const QList<QContactLocalId> &sourceContactIds)
{
    QMultiMap<QContactLocalId, QContactLocalId> mergeIds;

    Q_FOREACH (QContactLocalId id, sourceContactIds) {
        mergeIds.insert (contactTarget.localId(), id);
    }

    QctContactMergeRequest *mergeRequest = new QctContactMergeRequest();
    mergeRequest->setMergeIds(mergeIds);
    startRequest(mergeRequest);
}

void TestTelepathyPlugin::startRequest(QContactAbstractRequest *request)
{
    connect(request,
            SIGNAL(resultsAvailable()),
            SLOT(onRequestFinished()));
    request->setManager(mContactManager);
    QVERIFY(request->start());
}

void TestTelepathyPlugin::onRequestFinished()
{
    QContactAbstractRequest *request = qobject_cast<QContactAbstractRequest *>(sender());
    QVERIFY(request != 0);
    QVERIFY(request->isFinished());
    QCOMPARE(request->error(), QContactManager::NoError);
    request->deleteLater();
}

void TestTelepathyPlugin::onContactsFetched()
{
    QContactFetchRequest *req = qobject_cast<QContactFetchRequest *>(sender());
    QVERIFY(req);
    QVERIFY(req->isFinished());
    QCOMPARE (req->error(), QContactManager::NoError);
    QVERIFY(req->contacts().count() >= 1);
}

#define LETTERS "abcdefghijklmnopqrstuvwxyz"
#define NUMBERS "0123456789"
#define ALNUM LETTERS NUMBERS
#define TEXT ALNUM " "

static gchar *randomString(int len, const gchar *chars = LETTERS)
{
    gchar *result = g_new0(gchar, len + 1);
    for (int i = 0; i < len; i++) {
        result[i] = chars[qrand() % strlen(chars)];
    }

    return result;
}

#define N_CONTACTS 1000

void TestTelepathyPlugin::testBenchmark()
{
    /* create lots of new contacts */
    GArray *handles = g_array_new(FALSE, FALSE, sizeof(TpHandle));
    for (int i = 0; i < N_CONTACTS; i++) {
        TpHandle handle = ensureContact(randomString(20));
        g_array_append_val(handles, handle);
    }
    test_contact_list_manager_request_subscription(mListManager,
            handles->len, (TpHandle *) handles->data, "wait");
    TestExpectationMass exp(N_CONTACTS, 0, 0);
    runExpectation(&exp);

    /* Set account offline */
    tp_cli_connection_call_disconnect(mConnection, -1, NULL, NULL, NULL, NULL);

    TestExpectationDisconnect exp2(mLocalContactIds.count());
    runExpectation(&exp2);
}

TpHandle TestTelepathyPlugin::ensureContact(const gchar *id)
{
    TpHandleRepoIface *serviceRepo =
        tp_base_connection_get_handles(mConnService, TP_HANDLE_TYPE_CONTACT);

    TpHandle handle = tp_handle_ensure(serviceRepo, id, NULL, NULL);

    return handle;
}

void TestTelepathyPlugin::runExpectation(TestExpectation *exp)
{
    QVERIFY(mExpectation == 0);
    mExpectation = exp;
    mExpectation->setContactManager(mContactManager);
    connect(mExpectation, SIGNAL(finished()),
            mLoop, SLOT(quit()));

    QCOMPARE(mLoop->exec(), 0);

    mExpectation = 0;
}

void TestTelepathyPlugin::contactsAdded(const QList<QContactLocalId>& contactIds)
{
    debug() << "Got contactsAdded";
    Q_FOREACH (const QContactLocalId &id, contactIds) {
        QVERIFY(!mLocalContactIds.contains(id));
        mLocalContactIds << id;
    }
    verify(EventAdded, contactIds);
}

void TestTelepathyPlugin::contactsChanged(const QList<QContactLocalId>& contactIds)
{
    debug() << "Got contactsChanged";
    Q_FOREACH (const QContactLocalId &id, contactIds) {
        QVERIFY(mLocalContactIds.contains(id));
    }
    verify(EventChanged, contactIds);
}

void TestTelepathyPlugin::contactsRemoved(const QList<QContactLocalId>& contactIds)
{
    debug() << "Got contactsRemoved";
    Q_FOREACH (const QContactLocalId &id, contactIds) {
        QVERIFY(mLocalContactIds.contains(id));
        mLocalContactIds.removeOne(id);
    }
    verify(EventRemoved, contactIds);
}

void TestTelepathyPlugin::verify(Event event,
    const QList<QContactLocalId> &contactIds)
{
    QVERIFY(mExpectation != 0);
    mExpectation->verify(event, contactIds);
}

QTEST_MAIN(TestTelepathyPlugin)
