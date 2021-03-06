/*
Copyright (C) 2010 Srivats P.

This file is part of "Ostinato"

This is free software: you can redistribute it and/or modify
it under the terms of the GNU General Public License as published by
the Free Software Foundation, either version 3 of the License, or
(at your option) any later version.

This program is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
GNU General Public License for more details.

You should have received a copy of the GNU General Public License
along with this program.  If not, see <http://www.gnu.org/licenses/>
*/

#include "pcapport.h"

#include "devicemanager.h"
#include "packetbuffer.h"

#include <QtGlobal>

#ifdef Q_OS_WIN32
#include <windows.h>
#endif

pcap_if_t *PcapPort::deviceList_ = NULL;


#if defined(Q_OS_LINUX)
typedef struct timeval TimeStamp;
static void inline getTimeStamp(TimeStamp *stamp)
{
    gettimeofday(stamp, NULL);
}

// Returns time diff in usecs between end and start
static long inline udiffTimeStamp(const TimeStamp *start, const TimeStamp *end)
{
    struct timeval diff;
    long usecs;

    timersub(end, start, &diff);

    usecs = diff.tv_usec;
    if (diff.tv_sec)
        usecs += diff.tv_sec*1e6;

    return usecs;
}
#elif defined(Q_OS_WIN32)
static quint64 gTicksFreq;
typedef LARGE_INTEGER TimeStamp;
static void inline getTimeStamp(TimeStamp* stamp)
{
    QueryPerformanceCounter(stamp);
}

static long inline udiffTimeStamp(const TimeStamp *start, const TimeStamp *end)
{
    if (end->QuadPart >= start->QuadPart)
        return (end->QuadPart - start->QuadPart)*long(1e6)/gTicksFreq;
    else
    {
        // FIXME: incorrect! what's the max value for this counter before
        // it rolls over?
        return (start->QuadPart)*long(1e6)/gTicksFreq;
    }
}
#else
typedef int TimeStamp;
static void inline getTimeStamp(TimeStamp*) {}
static long inline udiffTimeStamp(const TimeStamp*, const TimeStamp*) { return 0; }
#endif

PcapPort::PcapPort(int id, const char *device)
    : AbstractPort(id, device)
{
    monitorRx_ = new PortMonitor(device, kDirectionRx, &stats_);
    monitorTx_ = new PortMonitor(device, kDirectionTx, &stats_);
    transmitter_ = new PortTransmitter(device);
    capturer_ = new PortCapturer(device);
    emulXcvr_ = new EmulationTransceiver(device, deviceManager_);

    if (!monitorRx_->handle() || !monitorTx_->handle())
        isUsable_ = false;

    if (!deviceList_)
    {
        char errbuf[PCAP_ERRBUF_SIZE];

        if (pcap_findalldevs(&deviceList_, errbuf) == -1)
            qDebug("Error in pcap_findalldevs_ex: %s\n", errbuf);
    }

    for (pcap_if_t *dev = deviceList_; dev != NULL; dev = dev->next)
    {
        if (strcmp(device, dev->name) == 0)
        {
            if (dev->name)
                data_.set_name(dev->name);
            if (dev->description)
                data_.set_description(dev->description);

            //! \todo set port IP addr also
        }
    }
}

void PcapPort::init()
{
    if (!monitorTx_->isDirectional())
        transmitter_->useExternalStats(&stats_);

    transmitter_->setHandle(monitorRx_->handle());

    updateNotes();

    monitorRx_->start();
    monitorTx_->start();
}

PcapPort::~PcapPort()
{
    qDebug("In %s", __FUNCTION__);

    if (monitorRx_)
        monitorRx_->stop();
    if (monitorTx_)
        monitorTx_->stop();

    delete emulXcvr_;
    delete capturer_;
    delete transmitter_;

    if (monitorRx_)
        monitorRx_->wait();
    delete monitorRx_;

    if (monitorTx_)
        monitorTx_->wait();
    delete monitorTx_;
}

void PcapPort::updateNotes()
{
    QString notes;

    if ((!monitorRx_->isPromiscuous()) || (!monitorTx_->isPromiscuous()))
        notes.append("<li>Non Promiscuous Mode</li>");

    if (!monitorRx_->isDirectional() && !hasExclusiveControl())
        notes.append("<li><i>Rx Frames/Bytes</i>: Includes non Ostinato Tx pkts also (Tx by Ostinato are not included)</li>");

    if (!monitorTx_->isDirectional() && !hasExclusiveControl())
        notes.append("<li><i>Tx Frames/Bytes</i>: Only Ostinato Tx pkts (Tx by others NOT included)</li>");

    if (notes.isEmpty())
        data_.set_notes("");
    else
        data_.set_notes(QString("<b>Limitation(s)</b>"
            "<ul>%1</ul>"
            "Rx/Tx Rates are also subject to above limitation(s)").
            arg(notes).toStdString());
}

bool PcapPort::setRateAccuracy(AbstractPort::Accuracy accuracy)
{
    if (transmitter_->setRateAccuracy(accuracy)) {
        AbstractPort::setRateAccuracy(accuracy);
        return true;
    }
    return false;
}

void PcapPort::startDeviceEmulation()
{
    emulXcvr_->start();
}

void PcapPort::stopDeviceEmulation()
{
    emulXcvr_->stop();
}

int PcapPort::sendEmulationPacket(PacketBuffer *pktBuf)
{
    return emulXcvr_->transmitPacket(pktBuf);
}

/*
 * ------------------------------------------------------------------- *
 * Port Monitor
 * ------------------------------------------------------------------- *
 */
PcapPort::PortMonitor::PortMonitor(const char *device, Direction direction,
        AbstractPort::PortStats *stats)
{
    int ret;
    char errbuf[PCAP_ERRBUF_SIZE] = "";
    bool noLocalCapture;

    direction_ = direction;
    isDirectional_ = true;
    isPromisc_ = true;
    noLocalCapture = true;
    stats_ = stats;
    stop_ = false;

_retry:
#ifdef Q_OS_WIN32
    int flags = 0;

    if (isPromisc_)
        flags |= PCAP_OPENFLAG_PROMISCUOUS;
    if (noLocalCapture)
        flags |= PCAP_OPENFLAG_NOCAPTURE_LOCAL;

    handle_ = pcap_open(device, 64 /* FIXME */, flags,
                1000 /* ms */, NULL, errbuf);
#else
    handle_ = pcap_open_live(device, 64 /* FIXME */, int(isPromisc_),
                1000 /* ms */, errbuf);
#endif

    if (handle_ == NULL)
    {
        if (isPromisc_ && QString(errbuf).contains("promiscuous"))
        {
            qDebug("Can't set promiscuous mode, trying non-promisc %s", device);
            isPromisc_ = false;
            goto _retry;
        }
        else if (noLocalCapture && QString(errbuf).contains("loopback"))
        {
            qDebug("Can't set no local capture mode %s", device);
            noLocalCapture = false;
            goto _retry;
        }
        else
            goto _open_error;
    }
#ifdef Q_OS_WIN32
    // pcap_setdirection() API is not supported in Windows.
    // NOTE: WinPcap 4.1.1 and above exports a dummy API that returns -1
    // but since we would like to work with previous versions of WinPcap
    // also, we assume the API does not exist
    ret = -1;
#else
    switch (direction_)
    {
    case kDirectionRx:
        ret = pcap_setdirection(handle_, PCAP_D_IN);
        break;
    case kDirectionTx:
        ret = pcap_setdirection(handle_, PCAP_D_OUT);
        break;
    default:
        ret = -1; // avoid 'may be used uninitialized' warning
        Q_ASSERT(false);
    }
#endif

    if (ret < 0)
        goto _set_direction_error;

    return;

_set_direction_error:
    qDebug("Error setting direction(%d) %s: %s\n", direction, device,
            pcap_geterr(handle_));
    isDirectional_ = false;
    return;

_open_error:
    qDebug("%s: Error opening port %s: %s\n", __FUNCTION__, device, errbuf);
}

PcapPort::PortMonitor::~PortMonitor()
{
    if (handle_)
        pcap_close(handle_);
}

void PcapPort::PortMonitor::run()
{
    while (!stop_)
    {
        int ret;
        struct pcap_pkthdr *hdr;
        const uchar *data;

        ret = pcap_next_ex(handle_, &hdr, &data);
        switch (ret)
        {
            case 1:
                switch (direction_)
                {
                case kDirectionRx:
                    stats_->rxPkts++;
                    stats_->rxBytes += hdr->len;
                    break;

                case kDirectionTx:
                    if (isDirectional_)
                    {
                        stats_->txPkts++;
                        stats_->txBytes += hdr->len;
                    }
                    break;

                default:
                    Q_ASSERT(false);
                }

                //! \todo TODO pkt/bit rates
                break;
            case 0:
                //qDebug("%s: timeout. continuing ...", __PRETTY_FUNCTION__);
                continue;
            case -1:
                qWarning("%s: error reading packet (%d): %s",
                        __PRETTY_FUNCTION__, ret, pcap_geterr(handle_));
                break;
            case -2:
                qWarning("%s: error reading packet (%d): %s",
                        __PRETTY_FUNCTION__, ret, pcap_geterr(handle_));
                break;
            default:
                qFatal("%s: Unexpected return value %d", __PRETTY_FUNCTION__, ret);
        }
    }
}

void PcapPort::PortMonitor::stop()
{
    stop_ = true;
    pcap_breakloop(handle());
}

/*
 * ------------------------------------------------------------------- *
 * Port Transmitter
 * ------------------------------------------------------------------- *
 */
PcapPort::PortTransmitter::PortTransmitter(const char *device)
{
    char errbuf[PCAP_ERRBUF_SIZE] = "";

#ifdef Q_OS_WIN32
    LARGE_INTEGER   freq;
    if (QueryPerformanceFrequency(&freq))
        gTicksFreq = freq.QuadPart;
    else
        Q_ASSERT_X(false, "PortTransmitter::PortTransmitter",
                "This Win32 platform does not support performance counter");
#endif
    state_ = kNotStarted;
    returnToQIdx_ = -1;
    loopDelay_ = 0;
    stop_ = false;
    stats_ = new AbstractPort::PortStats;
    usingInternalStats_ = true;
    handle_ = pcap_open_live(device, 64 /* FIXME */, 0, 1000 /* ms */, errbuf);

    if (handle_ == NULL)
        goto _open_error;

    usingInternalHandle_ = true;

    return;

_open_error:
    qDebug("%s: Error opening port %s: %s\n", __FUNCTION__, device, errbuf);
    usingInternalHandle_ = false;
}

PcapPort::PortTransmitter::~PortTransmitter()
{
    if (usingInternalStats_)
        delete stats_;
    if (usingInternalHandle_)
        pcap_close(handle_);
}

bool PcapPort::PortTransmitter::setRateAccuracy(
        AbstractPort::Accuracy accuracy)
{
    switch (accuracy) {
    case kHighAccuracy:
        udelayFn_ = udelay;
        qWarning("%s: rate accuracy set to High - busy wait", __FUNCTION__);
        break;
    case kLowAccuracy:
        udelayFn_ = QThread::usleep;
        qWarning("%s: rate accuracy set to Low - usleep", __FUNCTION__);
        break;
    default:
        qWarning("%s: unsupported rate accuracy value %d", __FUNCTION__,
                accuracy);
        return false;
    }
    return true;
}

void PcapPort::PortTransmitter::clearPacketList()
{
    Q_ASSERT(!isRunning());
    // \todo lock for packetSequenceList
    while(packetSequenceList_.size())
        delete packetSequenceList_.takeFirst();

    currentPacketSequence_ = NULL;
    repeatSequenceStart_ = -1;
    repeatSize_ = 0;
    packetCount_ = 0;

    returnToQIdx_ = -1;

    setPacketListLoopMode(false, 0, 0);
}

void PcapPort::PortTransmitter::loopNextPacketSet(qint64 size, qint64 repeats,
        long repeatDelaySec, long repeatDelayNsec)
{
    currentPacketSequence_ = new PacketSequence;
    currentPacketSequence_->repeatCount_ = repeats;
    currentPacketSequence_->usecDelay_ = repeatDelaySec * long(1e6)
                                            + repeatDelayNsec/1000;

    repeatSequenceStart_ = packetSequenceList_.size();
    repeatSize_ = size;
    packetCount_ = 0;

    packetSequenceList_.append(currentPacketSequence_);
}

bool PcapPort::PortTransmitter::appendToPacketList(long sec, long nsec,
        const uchar *packet, int length)
{
    bool op = true;
    pcap_pkthdr pktHdr;

    pktHdr.caplen = pktHdr.len = length;
    pktHdr.ts.tv_sec = sec;
    pktHdr.ts.tv_usec = nsec/1000;

    if (currentPacketSequence_ == NULL ||
            !currentPacketSequence_->hasFreeSpace(2*sizeof(pcap_pkthdr)+length))
    {
        if (currentPacketSequence_ != NULL)
        {
            long usecs;

            usecs = (pktHdr.ts.tv_sec
                        - currentPacketSequence_->lastPacket_->ts.tv_sec)
                            * long(1e6);
            usecs += (pktHdr.ts.tv_usec
                        - currentPacketSequence_->lastPacket_->ts.tv_usec);
            currentPacketSequence_->usecDelay_ = usecs;
        }

        //! \todo (LOW): calculate sendqueue size
        currentPacketSequence_ = new PacketSequence;

        packetSequenceList_.append(currentPacketSequence_);

        // Validate that the pkt will fit inside the new currentSendQueue_
        Q_ASSERT(currentPacketSequence_->hasFreeSpace(
                    sizeof(pcap_pkthdr) + length));
    }

    if (currentPacketSequence_->appendPacket(&pktHdr, (u_char*) packet) < 0)
    {
        op = false;
    }

    packetCount_++;
    if (repeatSize_ > 0 && packetCount_ == repeatSize_)
    {
        qDebug("repeatSequenceStart_=%d, repeatSize_ = %llu",
                repeatSequenceStart_, repeatSize_);

        // Set the packetSequence repeatSize
        Q_ASSERT(repeatSequenceStart_ >= 0);
        Q_ASSERT(repeatSequenceStart_ < packetSequenceList_.size());

        if (currentPacketSequence_ != packetSequenceList_[repeatSequenceStart_])
        {
            PacketSequence *start = packetSequenceList_[repeatSequenceStart_];

            currentPacketSequence_->usecDelay_ = start->usecDelay_;
            start->usecDelay_ = 0;
            start->repeatSize_ =
                    packetSequenceList_.size() - repeatSequenceStart_;
        }

        repeatSize_ = 0;

        // End current pktSeq and trigger a new pktSeq allocation for next pkt
        currentPacketSequence_ = NULL;
    }

    return op;
}

void PcapPort::PortTransmitter::setHandle(pcap_t *handle)
{
    if (usingInternalHandle_)
        pcap_close(handle_);
    handle_ = handle;
    usingInternalHandle_ = false;
}

void PcapPort::PortTransmitter::useExternalStats(AbstractPort::PortStats *stats)
{
    if (usingInternalStats_)
        delete stats_;
    stats_ = stats;
    usingInternalStats_ = false;
}

void PcapPort::PortTransmitter::run()
{
    //! \todo (MED) Stream Mode - continuous: define before implement

    // NOTE1: We can't use pcap_sendqueue_transmit() directly even on Win32
    // 'coz of 2 reasons - there's no way of stopping it before all packets
    // in the sendQueue are sent out and secondly, stats are available only
    // when all packets have been sent - no periodic updates
    //
    // NOTE2: Transmit on the Rx Handle so that we can receive it back
    // on the Tx Handle to do stats
    //
    // NOTE3: Update pcapExtra counters - port TxStats will be updated in the
    // 'stats callback' function so that both Rx and Tx stats are updated
    // together

    const int kSyncTransmit = 1;
    int i;
    long overHead = 0; // overHead should be negative or zero

    qDebug("packetSequenceList_.size = %d", packetSequenceList_.size());
    if (packetSequenceList_.size() <= 0)
        goto _exit;

    for(i = 0; i < packetSequenceList_.size(); i++) {
        qDebug("sendQ[%d]: rptCnt = %d, rptSz = %d, usecDelay = %ld", i,
                packetSequenceList_.at(i)->repeatCount_,
                packetSequenceList_.at(i)->repeatSize_,
                packetSequenceList_.at(i)->usecDelay_);
        qDebug("sendQ[%d]: pkts = %ld, usecDuration = %ld", i,
                packetSequenceList_.at(i)->packets_,
                packetSequenceList_.at(i)->usecDuration_);
    }

    state_ = kRunning;
    i = 0;
    while (i < packetSequenceList_.size())
    {

_restart:
        int rptSz  = packetSequenceList_.at(i)->repeatSize_;
        int rptCnt = packetSequenceList_.at(i)->repeatCount_;

        for (int j = 0; j < rptCnt; j++)
        {
            for (int k = 0; k < rptSz; k++)
            {
                int ret;
                PacketSequence *seq = packetSequenceList_.at(i+k);
#ifdef Q_OS_WIN32
                TimeStamp ovrStart, ovrEnd;

                if (seq->usecDuration_ <= long(1e6)) // 1s
                {
                    getTimeStamp(&ovrStart);
                    ret = pcap_sendqueue_transmit(handle_,
                            seq->sendQueue_, kSyncTransmit);
                    if (ret >= 0)
                    {
                        stats_->txPkts += seq->packets_;
                        stats_->txBytes += seq->bytes_;

                        getTimeStamp(&ovrEnd);
                        overHead += seq->usecDuration_
                            - udiffTimeStamp(&ovrStart, &ovrEnd);
                        Q_ASSERT(overHead <= 0);
                    }
                    if (stop_)
                        ret = -2;
                }
                else
                {
                    ret = sendQueueTransmit(handle_, seq->sendQueue_,
                            overHead, kSyncTransmit);
                }
#else
                ret = sendQueueTransmit(handle_, seq->sendQueue_,
                            overHead, kSyncTransmit);
#endif

                if (ret >= 0)
                {
                    long usecs = seq->usecDelay_ + overHead;
                    if (usecs > 0)
                    {
                        (*udelayFn_)(usecs);
                        overHead = 0;
                    }
                    else
                        overHead = usecs;
                }
                else
                {
                    qDebug("error %d in sendQueueTransmit()", ret);
                    qDebug("overHead = %ld", overHead);
                    stop_ = false;
                    goto _exit;
                }
            }
        }

        // Move to the next Packet Set
        i += rptSz;
    }

    if (returnToQIdx_ >= 0)
    {
        long usecs = loopDelay_ + overHead;

        if (usecs > 0)
        {
            (*udelayFn_)(usecs);
            overHead = 0;
        }
        else
            overHead = usecs;

        i = returnToQIdx_;
        goto _restart;
    }

_exit:
    state_ = kFinished;
}

void PcapPort::PortTransmitter::start()
{
    // FIXME: return error
    if (state_ == kRunning) {
        qWarning("Transmit start requested but is already running!");
        return;
    }

    state_ = kNotStarted;
    QThread::start();

    while (state_ == kNotStarted)
        QThread::msleep(10);
}

void PcapPort::PortTransmitter::stop()
{
    if (state_ == kRunning) {
        stop_ = true;
        while (state_ == kRunning)
            QThread::msleep(10);
    }
    else {
        // FIXME: return error
        qWarning("Transmit stop requested but is not running!");
        return;
    }
}

bool PcapPort::PortTransmitter::isRunning()
{
    return (state_ == kRunning);
}

int PcapPort::PortTransmitter::sendQueueTransmit(pcap_t *p,
        pcap_send_queue *queue, long &overHead, int sync)
{
    TimeStamp ovrStart, ovrEnd;
    struct timeval ts;
    struct pcap_pkthdr *hdr = (struct pcap_pkthdr*) queue->buffer;
    char *end = queue->buffer + queue->len;

    ts = hdr->ts;

    getTimeStamp(&ovrStart);
    while((char*) hdr < end)
    {
        uchar *pkt = (uchar*)hdr + sizeof(*hdr);
        int pktLen = hdr->caplen;

        if (sync)
        {
            long usec = (hdr->ts.tv_sec - ts.tv_sec) * 1000000 +
                (hdr->ts.tv_usec - ts.tv_usec);

            getTimeStamp(&ovrEnd);

            overHead -= udiffTimeStamp(&ovrStart, &ovrEnd);
            Q_ASSERT(overHead <= 0);
            usec += overHead;
            if (usec > 0)
            {
                (*udelayFn_)(usec);
                overHead = 0;
            }
            else
                overHead = usec;

            ts = hdr->ts;
            getTimeStamp(&ovrStart);
        }

        Q_ASSERT(pktLen > 0);

        pcap_sendpacket(p, pkt, pktLen);
        stats_->txPkts++;
        stats_->txBytes += pktLen;

        // Step to the next packet in the buffer
        hdr = (struct pcap_pkthdr*) (pkt + pktLen);
        pkt = (uchar*) ((uchar*)hdr + sizeof(*hdr));

        if (stop_)
        {
            return -2;
        }
    }

    return 0;
}

void PcapPort::PortTransmitter::udelay(unsigned long usec)
{
#if defined(Q_OS_WIN32)
    LARGE_INTEGER tgtTicks;
    LARGE_INTEGER curTicks;

    QueryPerformanceCounter(&curTicks);
    tgtTicks.QuadPart = curTicks.QuadPart + (usec*gTicksFreq)/1000000;

    while (curTicks.QuadPart < tgtTicks.QuadPart)
        QueryPerformanceCounter(&curTicks);
#elif defined(Q_OS_LINUX)
    struct timeval delay, target, now;

    //qDebug("usec delay = %ld", usec);

    delay.tv_sec = 0;
    delay.tv_usec = usec;

    while (delay.tv_usec >= 1000000)
    {
        delay.tv_sec++;
        delay.tv_usec -= 1000000;
    }

    gettimeofday(&now, NULL);
    timeradd(&now, &delay, &target);

    do {
        gettimeofday(&now, NULL);
    } while (timercmp(&now, &target, <));
#else
    QThread::usleep(usec);
#endif
}

/*
 * ------------------------------------------------------------------- *
 * Port Capturer
 * ------------------------------------------------------------------- *
 */
PcapPort::PortCapturer::PortCapturer(const char *device)
{
    device_ = QString::fromAscii(device);
    stop_ = false;
    state_ = kNotStarted;

    if (!capFile_.open())
        qWarning("Unable to open temp cap file");

    qDebug("cap file = %s", capFile_.fileName().toAscii().constData());

    dumpHandle_ = NULL;
    handle_ = NULL;
}

PcapPort::PortCapturer::~PortCapturer()
{
    capFile_.close();
}

void PcapPort::PortCapturer::run()
{
    int flag = PCAP_OPENFLAG_PROMISCUOUS;
    char errbuf[PCAP_ERRBUF_SIZE] = "";
    struct bpf_program fp;
    bpf_u_int32 net, mask;
    int looping = 0;

    qDebug("In %s", __PRETTY_FUNCTION__);

    if (!capFile_.isOpen())
    {
        qWarning("temp cap file is not open");
        goto _exit;
    }

    if (-1 == pcap_lookupnet(device_.toAscii().constData(), &net, &mask,errbuf))
    {
        net = 0;
        mask = 0;
    }
_retry:
    handle_ = pcap_open_live(device_.toAscii().constData(), 65535,
                    flag, 1000 /* ms */, errbuf);

    if (handle_ == NULL)
    {
        if (flag && QString(errbuf).contains("promiscuous"))
        {
            qDebug("%s:can't set promiscuous mode, trying non-promisc",
                    device_.toAscii().constData());
            flag = 0;
            goto _retry;
        }
        else
        {
            qDebug("%s: Error opening port %s: %s\n", __FUNCTION__,
                    device_.toAscii().constData(), errbuf);
            goto _exit;
        }
    }

    if (-1 == pcap_compile(handle_, &fp, filter_.toAscii().constData(), 0, mask))
    {
            qDebug("%s:can't compile BPF program: %s (%s)",
                    device_.toAscii().constData(),
                    filter_.toAscii().constData(),
                    pcap_geterr(handle_));
            goto _exit;
    }
    if (-1 == pcap_setfilter(handle_, &fp))
    {
            qDebug("%s:can't apply filter: %s (%s)",
                    device_.toAscii().constData(),
                    filter_.toAscii().constData(),
                    pcap_geterr(handle_));
            goto _exit;
    }
    pcap_freecode(&fp);
    pcap_setnonblock(handle_, 1, errbuf);

    dumpHandle_ = pcap_dump_open(handle_,
            capFile_.fileName().toAscii().constData());
    state_ = kRunning;
    looping = 1;
    while (looping)
    {
        int ret;

        ret = pcap_loop(handle_, 1000, pcap_dump, (uchar *)dumpHandle_);
        switch (ret)
        {
            case 0:
                // 1000 continuous packets processed, can do next 1000
                break;
            case -1:
                qWarning("%s: error reading packet (%d): %s",
                        __PRETTY_FUNCTION__, ret, pcap_geterr(handle_));
                looping = 0;
                break;
            case -2:
                qDebug("user requested capture stop\n");
                looping = 0;
                break;
            default:
                qFatal("%s: Unexpected return value %d", __PRETTY_FUNCTION__, ret);
                looping = 0;
        }
    }
    pcap_dump_close(dumpHandle_);
    pcap_close(handle_);
    dumpHandle_ = NULL;
    handle_ = NULL;
    stop_ = false;

_exit:
    state_ = kFinished;
}

void PcapPort::PortCapturer::start(const char *filter)
{
    // FIXME: return error
    if (state_ == kRunning) {
        qWarning("Capture start requested but is already running!");
        return;
    }
    filter_ = QString::fromAscii(filter);

    state_ = kNotStarted;
    QThread::start();

    while (state_ == kNotStarted)
        QThread::msleep(10);
}

void PcapPort::PortCapturer::stop()
{
    if (state_ == kRunning) {
        stop_ = true;
        pcap_breakloop(handle_);
        while (state_ == kRunning) {
            qDebug("capture stoping...\n");
            QThread::msleep(500);
        }
    }
    else {
        // FIXME: return error
        qWarning("Capture stop requested but is not running!");
        return;
    }
}

bool PcapPort::PortCapturer::isRunning()
{
    return (state_ == kRunning);
}

QFile* PcapPort::PortCapturer::captureFile()
{
    return &capFile_;
}


/*
 * ------------------------------------------------------------------- *
 * Transmit+Receiver for Device/ProtocolEmulation
 * ------------------------------------------------------------------- *
 */
PcapPort::EmulationTransceiver::EmulationTransceiver(const char *device,
        DeviceManager *deviceManager)
{
    device_ = QString::fromAscii(device);
    deviceManager_ = deviceManager;
    stop_ = false;
    state_ = kNotStarted;
    handle_ = NULL;
}

PcapPort::EmulationTransceiver::~EmulationTransceiver()
{
    stop();
}

void PcapPort::EmulationTransceiver::run()
{
    int flags = PCAP_OPENFLAG_PROMISCUOUS;
    char errbuf[PCAP_ERRBUF_SIZE] = "";
    struct bpf_program bpf;
#if 0
    const char *capture_filter =
        "arp or icmp or icmp6 or "
        "(vlan and (arp or icmp or icmp6)) or "
        "(vlan and vlan and (arp or icmp or icmp6)) or "
        "(vlan and vlan and vlan and (arp or icmp or icmp6)) or "
        "(vlan and vlan and vlan and vlan and (arp or icmp or icmp6))";
/*
    Ideally we should use the above filter, but the 'vlan' capture filter
    in libpcap is implemented as a kludge. From the pcap-filter man page -

    vlan [vlan_id]
       Note that the first vlan keyword encountered in expression changes
       the decoding offsets for the remainder of expression on the
       assumption that the packet is a VLAN packet.

       The  vlan [vlan_id] expression may be used more than once, to filter on
       VLAN hierarchies. Each use of that expression increments the filter
       offsets by 4.

    See https://ask.wireshark.org/questions/31953/unusual-behavior-with-stacked-vlan-tags-and-capture-filter

    So we use the modified filter expression that works as we intend. If ever
    libpcap changes their implementation, this will need to change as well.
*/
#else
    const char *capture_filter =
        "arp or icmp or icmp6 or "
        "(vlan and (arp or icmp or icmp6)) or "
        "(vlan and (arp or icmp or icmp6)) or "
        "(vlan and (arp or icmp or icmp6)) or "
        "(vlan and (arp or icmp or icmp6))";
#endif

    const int optimize = 1;

    qDebug("In %s", __PRETTY_FUNCTION__);

#ifdef Q_OS_WIN32
    flags |= PCAP_OPENFLAG_NOCAPTURE_LOCAL;
#endif

#ifdef Q_OS_WIN32
_retry:
    // NOCAPTURE_LOCAL needs windows only pcap_open()
    handle_ = pcap_open(qPrintable(device_), 65535,
                flags, 100 /* ms */, NULL, errbuf);
#else
    handle_ = pcap_open_live(qPrintable(device_), 65535,
                    flags, 100 /* ms */, errbuf);
#endif

    if (handle_ == NULL)
    {
        if (flags && QString(errbuf).contains("promiscuous"))
        {
            notify("Unable to set promiscuous mode on <%s> - "
                    "device emulation will not work", qPrintable(device_));
            goto _exit;
        }
#ifdef Q_OS_WIN32
        else if ((flags & PCAP_OPENFLAG_NOCAPTURE_LOCAL)
                && QString(errbuf).contains("loopback"))
        {
            qDebug("Can't set no local capture mode %s", qPrintable(device_));
            flags &= ~PCAP_OPENFLAG_NOCAPTURE_LOCAL;
            goto _retry;
        }
#endif
        else
        {
            notify("Unable to open <%s> [%s] - device emulation will not work",
                    qPrintable(device_), errbuf);
            goto _exit;
        }
    }

    // TODO: for now the filter is hardcoded to accept tagged/untagged
    // ARP/NDP or ICMPv4/v6; when more protocols are added, we may need
    // to derive this filter based on which protocols are configured
    // on the devices
    if (pcap_compile(handle_, &bpf, capture_filter, optimize, 0) < 0)
    {
        qWarning("%s: error compiling filter: %s", qPrintable(device_),
                pcap_geterr(handle_));
        goto _skip_filter;
    }

    if (pcap_setfilter(handle_, &bpf) < 0)
    {
        qWarning("%s: error setting filter: %s", qPrintable(device_),
                pcap_geterr(handle_));
        goto _skip_filter;
    }

_skip_filter:
    state_ = kRunning;
    while (1)
    {
        int ret;
        struct pcap_pkthdr *hdr;
        const uchar *data;

        ret = pcap_next_ex(handle_, &hdr, &data);
        switch (ret)
        {
            case 1:
            {
                PacketBuffer *pktBuf = new PacketBuffer(data, hdr->caplen);
#if 0
                for (int i = 0; i < 64; i++) {
                    printf("%02x ", data[i]);
                    if (i % 16 == 0)
                        printf("\n");
                }
                printf("\n");
#endif
                // XXX: deviceManager should free pktBuf before returning
                // from this call; if it needs to process the pkt async
                // it should make a copy as the pktBuf's data buffer is
                // owned by libpcap which does not guarantee data will
                // persist across calls to pcap_next_ex()
                deviceManager_->receivePacket(pktBuf);
                break;
            }
            case 0:
                // timeout: just go back to the loop
                break;
            case -1:
                qWarning("%s: error reading packet (%d): %s",
                        __PRETTY_FUNCTION__, ret, pcap_geterr(handle_));
                break;
            case -2:
            default:
                qFatal("%s: Unexpected return value %d", __PRETTY_FUNCTION__,
                        ret);
        }

        if (stop_)
        {
            qDebug("user requested receiver stop\n");
            break;
        }
    }
    pcap_close(handle_);
    handle_ = NULL;
    stop_ = false;

_exit:
    state_ = kFinished;
}

void PcapPort::EmulationTransceiver::start()
{
    if (state_ == kRunning) {
        qWarning("Receive start requested but is already running!");
        return;
    }

    state_ = kNotStarted;
    QThread::start();

    while (state_ == kNotStarted)
        QThread::msleep(10);
}

void PcapPort::EmulationTransceiver::stop()
{
    if (state_ == kRunning) {
        stop_ = true;
        while (state_ == kRunning)
            QThread::msleep(10);
    }
    else {
        qWarning("Receive stop requested but is not running!");
        return;
    }
}

bool PcapPort::EmulationTransceiver::isRunning()
{
    return (state_ == kRunning);
}

int PcapPort::EmulationTransceiver::transmitPacket(PacketBuffer *pktBuf)
{
    return pcap_sendpacket(handle_, pktBuf->data(), pktBuf->length());
}
