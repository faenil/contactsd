/* * This file is part of contacts *
 * Copyright © 2009 Nokia Corporation and/or its subsidiary(-ies). All rights reserved.
 * Contact:  Aleksandar Stojiljkovic <aleksandar.stojiljkovic@nokia.com>
 * This software, including documentation, is protected by copyright controlled by
 * Nokia Corporation. All rights are reserved. Copying, including reproducing, storing,
 * adapting or translating, any or all of this material requires the prior written consent
 * of Nokia Corporation. This material also contains confidential information which may
 * not be disclosed to others without the prior written consent of Nokia. */

#include "trackersink.h"
#include <TelepathyQt4/Types>
#include <TelepathyQt4/Constants>
#include <TelepathyQt4/ContactManager>
#include <TelepathyQt4/Connection>

#include <contactphotocopy.h>

//tracker headers

class TrackerSink::Private
{
public:
    Private():transaction_(0) {}
    ~Private() {
    }
    QHash<uint, QSharedPointer<TpContact> > contactMap;
    RDFTransactionPtr transaction_;
    QHash<uint, uint> presenceHash; // maps tpCId hash to presence message hash
    LiveNodes livenode;

};

TrackerSink* TrackerSink::instance()
{
    static TrackerSink instance_;
    return &instance_;
}

TrackerSink::TrackerSink(): d(new Private)
{
}

TrackerSink::~TrackerSink()
{
    delete d;
}

void TrackerSink::getIMContacts(const QString& /* contact_iri */)
{
    RDFSelect select;

    RDFVariable contact = RDFVariable::fromType<nco::PersonContact>();
    RDFVariable imaddress = contact.property<nco::hasIMAddress>();

    select.addColumn("distinct", imaddress.property<nco::imID>());
    select.addColumn("contactId", contact.property<nco::contactLocalUID> ());

    d->livenode = ::tracker()->modelQuery(select);

    connect(d->livenode.model(), SIGNAL(modelUpdated()), this, SLOT(onModelUpdate()));

}

void TrackerSink::onModelUpdate()
{
    QStringList idList;

    for (int i = 0 ; i < d->livenode->rowCount() ; i ++) {
        const QString imAddress = d->livenode->index(i, 0).data().toString();
        const QString imLocalId = d->livenode->index(i, 1).data().toString();

        bool ok;
        const uint localId = imLocalId.toUInt(&ok, 0);

        const QSharedPointer<const TpContact> contact = d->contactMap[localId];
        qDebug() << imAddress << imLocalId << ":" << contact;

        if (0 == contact) {
            qWarning() << "cannot find TpContact for" << imAddress << localId;
            continue;
        }

        const QSharedPointer<const Tp::Contact> tcontact = contact->contact();
        saveToTracker(imLocalId, tcontact->id(),
                      tcontact->alias(),
                      tcontact->presenceStatus(),
                      tcontact->presenceMessage(),
                      contact->accountPath(),
                      tcontact->capabilities());
    }


    if (d->livenode->rowCount() <= 0) {
        foreach (const QSharedPointer<TpContact>& contact, d->contactMap.values()) {
            const QSharedPointer<const Tp::Contact> tcontact = contact->contact();
            const QString tpCId = contact->accountPath() + "!" + tcontact->id();
            const QString id(QString::number(qHash(tpCId)));

            if(!tcontact)
                continue;

            saveToTracker(id, tcontact->id(),
                          tcontact->alias(),
                          tcontact->presenceStatus(),
                          tcontact->presenceMessage(),
                          contact->accountPath(),
                          tcontact->capabilities());

        }
    }

}

void TrackerSink::connectOnSignals(TpContactPtr contact)
{
    connect(contact.data(), SIGNAL(change(uint, TpContact::ChangeType)),
            this, SLOT(onChange(uint, TpContact::ChangeType)));
}

TpContact* TrackerSink::find(uint id)
{
    if (d->contactMap.keys().contains(id) ) {
        return d->contactMap[id].data();
    }

    return 0;
}

void TrackerSink::onChange(uint uniqueId, TpContact::ChangeType type)
{
    TpContact* contact = find(uniqueId);
    if (!contact) {
        return;
    }

    switch (type) {
    case TpContact::AVATAR_TOKEN:
        onAvatarUpdated(contact->contact()->id(), contact->avatar(), contact->avatarMime());
        break;
    case TpContact::SIMPLE_PRESENCE:
        onSimplePresenceChanged(contact, uniqueId);
        break;
    case TpContact::CAPABILITIES:
        onCapabilities(contact);
        break;
    case TpContact::FEATURES:
        qDebug() << Q_FUNC_INFO;
        break;
    }
}
void TrackerSink::onFeaturesReady(TpContact* tpContact)
{
    foreach (TpContactPtr contact, d->contactMap) {
        if (contact.data() == tpContact) {
            sinkToStorage(contact);
        }
    }
}

static QUrl buildContactIri(const QString& uniqueIdStr)
{
  return QUrl("contact:" + uniqueIdStr);
}

static QUrl buildContactIri(unsigned int uniqueId)
{
  return buildContactIri(QString::number(uniqueId));
}

void TrackerSink::saveToTracker(const QString& uri, const QString& imId, const QString& nick, const QString& status, const QString& msg, const QString& accountpath, Tp::ContactCapabilities * contactcaps)
{
    const RDFVariable contact(buildContactIri(uri));

    const QString id(QString::number(TpContact::buildUniqueId(accountpath, imId)));
    const RDFVariable imAddress(TpContact::buildImAddress(accountpath, imId));
    const RDFVariable imAccount(QUrl("telepathy:" + accountpath));

    qDebug() << Q_FUNC_INFO <<id;

    RDFUpdate addressUpdate;

    addressUpdate.addDeletion(imAddress, nco::imNickname::iri());
    addressUpdate.addDeletion(imAddress, nco::imPresence::iri());
    addressUpdate.addDeletion(imAddress, nco::imStatusMessage::iri());

    addressUpdate.addInsertion(RDFStatementList() <<
                               RDFStatement(imAddress, rdf::type::iri(), nco::IMAddress::iri()) <<
                               RDFStatement(imAddress, nco::imNickname::iri(), LiteralValue(nick)) <<
                               RDFStatement(imAddress, nco::imStatusMessage::iri(), LiteralValue((msg))) <<
                               RDFStatement(imAddress, nco::imPresence::iri(), toTrackerStatus(status)) <<
                               RDFStatement(imAddress, nco::imID::iri(), LiteralValue(imId)) );

    addressUpdate.addInsertion(RDFStatementList() <<
                               RDFStatement(contact, rdf::type::iri(), nco::PersonContact::iri()) <<
                               RDFStatement(contact, nco::hasIMAddress::iri(), imAddress) <<
                               RDFStatement(contact, nco::contactLocalUID::iri(), LiteralValue(id)) );

    addressUpdate.addInsertion(RDFStatementList() <<
                               RDFStatement(imAccount, rdf::type::iri(), nco::IMAccount::iri()) <<
                               RDFStatement(imAccount, nco::hasIMContact::iri(), imAddress));

    addressUpdate.addDeletion(imAddress, nco::imCapability::iri());



    if (contactcaps->supportsMediaCalls() || contactcaps->supportsAudioCalls())  {
        addressUpdate.addInsertion( RDFStatementList() <<
                                    RDFStatement(imAddress, nco::imCapability::iri(),
                                                 nco::im_capability_audio_calls::iri()));

    }

    if (contactcaps->supportsTextChats() ) {
        addressUpdate.addInsertion( RDFStatementList() <<
                                    RDFStatement(imAddress, nco::imCapability::iri(),
                                                 nco::im_capability_text_chat::iri()));
    }

    service()->executeQuery(addressUpdate);

}

void TrackerSink::sinkToStorage(const QSharedPointer<TpContact>& obj)
{
    const unsigned int uniqueId = obj->uniqueId();
    if (!find(uniqueId) ) {
        connectOnSignals(obj);
        d->contactMap[uniqueId] = obj;
    }


    if (!obj->isReady()) {
        connect(obj.data(), SIGNAL(ready(TpContact*)),
                this, SLOT(onFeaturesReady(TpContact*)));
        return;
    }

    const QSharedPointer<const Tp::Contact> tcontact = obj->contact();

    qDebug() << Q_FUNC_INFO <<
        " \n{Contact Id :" << tcontact->id() <<
        " }\n{Alias : " <<  tcontact->alias() <<
        " }\n{Status : " << tcontact->presenceStatus() <<
        " }\n{Message : " << tcontact->presenceMessage() <<
        " }\n{AccountPath : " << obj->accountPath();

    const QString id(QString::number(uniqueId));

    saveToTracker(id, tcontact->id(),
                      tcontact->alias(),
                      tcontact->presenceStatus(),
                      tcontact->presenceMessage(),
                      obj->accountPath(),
                      tcontact->capabilities());

}

void TrackerSink::onCapabilities(TpContact* obj)
{
    if (!isValidTpContact(obj)) {
        qDebug() << Q_FUNC_INFO << "Invalid Telepathy Contact";
        return;
    }

    const RDFVariable imAddress(obj->imAddress());

    RDFUpdate addressUpdate;

    addressUpdate.addDeletion(imAddress, nco::imCapability::iri());

    const Tp::ContactCapabilities *capabilities = obj->contact()->capabilities();

    if (capabilities && (capabilities->supportsMediaCalls() ||
            capabilities->supportsAudioCalls()) ) {


        //TODO: Move this to the constructor, so it's only called once?
        // This liveNode() call actually sets this RDF triple:
        //   nco::im_capability_audio_calls a rdfs:Resource
        // though the libqtttracker maintainer agrees that it's bad API.
        Live<nco::IMCapability> cap =
            service()->liveNode(nco::im_capability_audio_calls::iri());

        addressUpdate.addInsertion( RDFStatementList() <<
                                    RDFStatement(imAddress, nco::imCapability::iri(),
                                                 nco::im_capability_audio_calls::iri()));

    }

    service()->executeQuery(addressUpdate);

    this->commitTrackerTransaction();
}


void TrackerSink::onSimplePresenceChanged(TpContact* obj, uint uniqueId)
{
    qDebug() << Q_FUNC_INFO;
    if (!isValidTpContact(obj)) {
        qDebug() << Q_FUNC_INFO << "Invalid Telepathy Contact";
        return;
    }

    const RDFVariable contact(buildContactIri(uniqueId));
    const RDFVariable imAddress(obj->imAddress());

    RDFUpdate addressUpdate;

    addressUpdate.addDeletion(imAddress, nco::imPresence::iri());
    addressUpdate.addDeletion(imAddress, nco::imStatusMessage::iri());
    addressUpdate.addDeletion(imAddress, nie::contentLastModified::iri());
    addressUpdate.addDeletion(contact, nie::contentLastModified::iri());

    const QSharedPointer<const Tp::Contact> tcontact = obj->contact();
    const QDateTime datetime = QDateTime::currentDateTime();

    RDFStatementList insertions;
    insertions << RDFStatement(imAddress, nco::imStatusMessage::iri(), LiteralValue(tcontact->presenceMessage()))
        << RDFStatement(imAddress, nie::contentLastModified::iri(), LiteralValue(datetime));
    insertions << RDFStatement(imAddress, nco::imPresence::iri(), toTrackerStatus(tcontact->presenceStatus()));
    addressUpdate.addInsertion(insertions);
    addressUpdate.addInsertion(contact, nie::contentLastModified::iri(), RDFVariable(datetime));

    service()->executeQuery(addressUpdate);

    this->commitTrackerTransaction();

}

QList<QSharedPointer<TpContact> > TrackerSink::getFromStorage()
{
    return  d->contactMap.values();
}

bool TrackerSink::contains(const QString& aId)
{
    if (aId.isEmpty()) {
        return false;
    }

    RDFVariable rdfContact = RDFVariable::fromType<nco::Contact>();
    rdfContact.property<nco::hasIMAccount>().property<nco::imID>() = LiteralValue(aId);

    RDFSelect query;
    query.addColumn(rdfContact);
    LiveNodes ncoContacts = ::tracker()->modelQuery(query);
    if (ncoContacts->rowCount() > 0) {
        return true;
    }

    return false;

}

bool TrackerSink::compareAvatar(const QString& token)
{
    Q_UNUSED(token)
    return false;
}


void TrackerSink::saveAvatarToken(const QString& id, const QString& token, const QString& mime)
{
    const QString avatarPath = ContactPhotoCopy::avatarDir()+"/telepathy_cache"+token+'.'+ mime;

    foreach (const QSharedPointer<const TpContact>& c, d->contactMap) {
        const QSharedPointer<const Tp::Contact> tcontact = c->contact();
        if (tcontact && id == tcontact->id() ) {
            using namespace SopranoLive;
            if (!isValidTpContact(c.data())) {
                continue;
            }

            const QUrl tpUrl = c->imAddress();
            Live<nco::IMAddress> address = service()->liveNode(tpUrl);

            //TODO: Can this be moved to later, where it is first used?
            // It has not been moved yet, because some
            // of these liveNode() calls actually set RDF triples,
            // though the libqtttracker maintainer agrees that it's bad API.
            Live<nie::InformationElement> info = service()->liveNode(tpUrl);

            const QString uniqueIdStr = QString::number(c->uniqueId());
            Live<nco::PersonContact> photoAccount = service()->liveNode(buildContactIri(uniqueIdStr));

            // set both properties for a transition period
            // TODO: Set just one when it has been decided:
            photoAccount->setContactLocalUID(uniqueIdStr);
            photoAccount->setContactUID(uniqueIdStr);

            const QDateTime datetime = QDateTime::currentDateTime();
            photoAccount->setContentCreated(datetime);
            info->setContentLastModified(datetime);

            //FIXME:
            //To be removed once tracker plugin reads imaddress photo url's
            Live<nie::DataObject> fileurl = ::tracker()->liveNode(QUrl(avatarPath));
            photoAccount->setPhoto(fileurl);
            address->addImAvatar(fileurl);

            break;
        }
    }
    this->commitTrackerTransaction();
}

void TrackerSink::onAvatarUpdated(const QString& id, const QString& token, const QString& mime)
{
    if (!compareAvatar(token))
        saveAvatarToken(id, token, mime);
}

//TODO: This seems to be useless:
bool TrackerSink::isValidTpContact(const TpContact *tpContact) {
    if (tpContact != 0) {
        return true;
    }

    return false;
}

RDFServicePtr TrackerSink::service()
{
    if (d->transaction_)
    {
        // if transaction was obtained, grab the service from inside it and use it
        return d->transaction_->service();
    }
    else
    {
        // otherwise, use tracker directly, with no transactions.
        return ::tracker();
    }
}

void TrackerSink::commitTrackerTransaction() {
    if ( d->transaction_)
    {
        d->transaction_->commit();
        d->transaction_ = RDFTransactionPtr(0); // that's it, transaction lives until commit
    }
}

void TrackerSink::initiateTrackerTransaction()
{
    if ( !d->transaction_ )
        d->transaction_ = ::tracker()->createTransaction();
}


/* When account goes offline make all contacts as Unknown
   this is a specification requirement
   */

void TrackerSink::takeAllOffline(const QString& path)
{

    initiateTrackerTransaction();
    RDFUpdate addressUpdate;
    foreach (TpContactPtr obj, getFromStorage()) {
        if (obj->accountPath() != path) {
            continue;
        }

        const RDFVariable contact(buildContactIri(obj->uniqueId()));
        const RDFVariable imAddress(obj->imAddress());

        addressUpdate.addDeletion(imAddress, nco::imPresence::iri());
        addressUpdate.addDeletion(imAddress, nco::imStatusMessage::iri());
        addressUpdate.addDeletion(contact, nie::contentLastModified::iri());

        const QLatin1String status("unknown");
        addressUpdate.addInsertion(RDFStatementList() <<
                                   RDFStatement(imAddress, nco::imStatusMessage::iri(),
                                                LiteralValue("")) <<
                                   RDFStatement(imAddress, nco::imPresence::iri(),
                                                toTrackerStatus(status)));

        addressUpdate.addInsertion(contact, nie::contentLastModified::iri(),
                                   RDFVariable(QDateTime::currentDateTime()));
    }

    service()->executeQuery(addressUpdate);
    this->commitTrackerTransaction();
}

const QUrl & TrackerSink::toTrackerStatus(const QString& status)
{
    static QHash<QString, QUrl> mapping;

    if (mapping.isEmpty()) {
        mapping.insert("offline", nco::presence_status_offline::iri());
        mapping.insert("available", nco::presence_status_available::iri());
        mapping.insert("away", nco::presence_status_away::iri());
        mapping.insert("xa", nco::presence_status_extended_away::iri());
        mapping.insert("dnd", nco::presence_status_busy::iri());
        mapping.insert("unknown", nco::presence_status_unknown::iri());
        mapping.insert("hidden", nco::presence_status_hidden::iri());
        mapping.insert("busy", nco::presence_status_busy::iri());
    }

    QHash<QString, QUrl>::const_iterator i(mapping.find(status));

    if (i != mapping.end()) {
        return *i;
    }

    return nco::presence_status_error::iri();
}
