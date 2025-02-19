#include "Limelight-internal.h"
#include "PlatformSockets.h"
#include "PlatformThreads.h"

#include "ByteBuffer.h"

#include <enet/enet.h>

#include <openssl/evp.h>

// NV control stream packet header for TCP
typedef struct _NVCTL_TCP_PACKET_HEADER {
    unsigned short type;
    unsigned short payloadLength;
} NVCTL_TCP_PACKET_HEADER, *PNVCTL_TCP_PACKET_HEADER;

typedef struct _NVCTL_ENET_PACKET_HEADER_V1 {
    unsigned short type;
} NVCTL_ENET_PACKET_HEADER_V1, *PNVCTL_ENET_PACKET_HEADER_V1;

typedef struct _NVCTL_ENET_PACKET_HEADER_V2 {
    unsigned short type;
    unsigned short payloadLength;
} NVCTL_ENET_PACKET_HEADER_V2, *PNVCTL_ENET_PACKET_HEADER_V2;

#define AES_GCM_TAG_LENGTH 16
typedef struct _NVCTL_ENCRYPTED_PACKET_HEADER {
    unsigned short encryptedHeaderType; // Always LE 0x0001
    unsigned short length; // sizeof(seq) + 16 byte tag + secondary header and data
    unsigned int seq; // Monotonically increasing sequence number (used as IV for AES-GCM)

    // encrypted NVCTL_ENET_PACKET_HEADER_V2 and payload data follow
} NVCTL_ENCRYPTED_PACKET_HEADER, *PNVCTL_ENCRYPTED_PACKET_HEADER;

typedef struct _QUEUED_FRAME_INVALIDATION_TUPLE {
    int startFrame;
    int endFrame;
    LINKED_BLOCKING_QUEUE_ENTRY entry;
} QUEUED_FRAME_INVALIDATION_TUPLE, *PQUEUED_FRAME_INVALIDATION_TUPLE;

static SOCKET ctlSock = INVALID_SOCKET;
static ENetHost* client;
static ENetPeer* peer;
static PLT_MUTEX enetMutex;
static bool usePeriodicPing;

static PLT_THREAD lossStatsThread;
static PLT_THREAD invalidateRefFramesThread;
static PLT_THREAD controlReceiveThread;
static PLT_EVENT invalidateRefFramesEvent;
static int lossCountSinceLastReport;
static int lastGoodFrame;
static int lastSeenFrame;
static bool stopping;
static bool disconnectPending;
static bool encryptedControlStream;

static int intervalGoodFrameCount;
static int intervalTotalFrameCount;
static uint64_t intervalStartTimeMs;
static int lastIntervalLossPercentage;
static int lastConnectionStatusUpdate;
static int currentEnetSequenceNumber;

static bool idrFrameRequired;
static LINKED_BLOCKING_QUEUE invalidReferenceFrameTuples;

static EVP_CIPHER_CTX* cipherContext;

#if OPENSSL_VERSION_NUMBER < 0x10100000L
#define EVP_CIPHER_CTX_reset(x) EVP_CIPHER_CTX_cleanup(x); EVP_CIPHER_CTX_init(x)
#endif

#define CONN_IMMEDIATE_POOR_LOSS_RATE 30
#define CONN_CONSECUTIVE_POOR_LOSS_RATE 15
#define CONN_OKAY_LOSS_RATE 5
#define CONN_STATUS_SAMPLE_PERIOD 3000

#define IDX_START_A 0
#define IDX_REQUEST_IDR_FRAME 0
#define IDX_START_B 1
#define IDX_INVALIDATE_REF_FRAMES 2
#define IDX_LOSS_STATS 3
#define IDX_INPUT_DATA 5
#define IDX_RUMBLE_DATA 6
#define IDX_TERMINATION 7

#define CONTROL_STREAM_TIMEOUT_SEC 10

static const short packetTypesGen3[] = {
    0x1407, // Request IDR frame
    0x1410, // Start B
    0x1404, // Invalidate reference frames
    0x140c, // Loss Stats
    0x1417, // Frame Stats (unused)
    -1,     // Input data (unused)
    -1,     // Rumble data (unused)
    -1,     // Termination (unused)
};
static const short packetTypesGen4[] = {
    0x0606, // Request IDR frame
    0x0609, // Start B
    0x0604, // Invalidate reference frames
    0x060a, // Loss Stats
    0x0611, // Frame Stats (unused)
    -1,     // Input data (unused)
    -1,     // Rumble data (unused)
    -1,     // Termination (unused)
};
static const short packetTypesGen5[] = {
    0x0305, // Start A
    0x0307, // Start B
    0x0301, // Invalidate reference frames
    0x0201, // Loss Stats
    0x0204, // Frame Stats (unused)
    0x0207, // Input data
    -1,     // Rumble data (unused)
    -1,     // Termination (unused)
};
static const short packetTypesGen7[] = {
    0x0305, // Start A
    0x0307, // Start B
    0x0301, // Invalidate reference frames
    0x0201, // Loss Stats
    0x0204, // Frame Stats (unused)
    0x0206, // Input data
    0x010b, // Rumble data
    0x0100, // Termination
};
static const short packetTypesGen7Enc[] = {
    0x0305, // Start A
    0x0307, // Start B
    0x0301, // Invalidate reference frames
    0x0201, // Loss Stats
    0x0204, // Frame Stats (unused)
    0x0206, // Input data
    0x010b, // Rumble data
    0x0109, // Termination (extended)
};

static const char requestIdrFrameGen3[] = { 0, 0 };
static const int startBGen3[] = { 0, 0, 0, 0xa };

static const char requestIdrFrameGen4[] = { 0, 0 };
static const char startBGen4[] = { 0 };

static const char startAGen5[] = { 0, 0 };
static const char startBGen5[] = { 0 };

static const short payloadLengthsGen3[] = {
    sizeof(requestIdrFrameGen3), // Request IDR frame
    sizeof(startBGen3), // Start B
    24, // Invalidate reference frames
    32, // Loss Stats
    64, // Frame Stats
    -1, // Input data
};
static const short payloadLengthsGen4[] = {
    sizeof(requestIdrFrameGen4), // Request IDR frame
    sizeof(startBGen4), // Start B
    24, // Invalidate reference frames
    32, // Loss Stats
    64, // Frame Stats
    -1, // Input data
};
static const short payloadLengthsGen5[] = {
    sizeof(startAGen5), // Start A
    sizeof(startBGen5), // Start B
    24, // Invalidate reference frames
    32, // Loss Stats
    80, // Frame Stats
    -1, // Input data
};
static const short payloadLengthsGen7[] = {
    sizeof(startAGen5), // Start A
    sizeof(startBGen5), // Start B
    24, // Invalidate reference frames
    32, // Loss Stats
    80, // Frame Stats
    -1, // Input data
};

static const char* preconstructedPayloadsGen3[] = {
    requestIdrFrameGen3,
    (char*)startBGen3
};
static const char* preconstructedPayloadsGen4[] = {
    requestIdrFrameGen4,
    startBGen4
};
static const char* preconstructedPayloadsGen5[] = {
    startAGen5,
    startBGen5
};
static const char* preconstructedPayloadsGen7[] = {
    startAGen5,
    startBGen5
};

static short* packetTypes;
static short* payloadLengths;
static char**preconstructedPayloads;

#define LOSS_REPORT_INTERVAL_MS 50
#define PERIODIC_PING_INTERVAL_MS 250

// Initializes the control stream
int initializeControlStream(void) {
    stopping = false;
    PltCreateEvent(&invalidateRefFramesEvent);
    LbqInitializeLinkedBlockingQueue(&invalidReferenceFrameTuples, 20);
    PltCreateMutex(&enetMutex);

    encryptedControlStream = APP_VERSION_AT_LEAST(7, 1, 431);

    if (AppVersionQuad[0] == 3) {
        packetTypes = (short*)packetTypesGen3;
        payloadLengths = (short*)payloadLengthsGen3;
        preconstructedPayloads = (char**)preconstructedPayloadsGen3;
    }
    else if (AppVersionQuad[0] == 4) {
        packetTypes = (short*)packetTypesGen4;
        payloadLengths = (short*)payloadLengthsGen4;
        preconstructedPayloads = (char**)preconstructedPayloadsGen4;
    }
    else if (AppVersionQuad[0] == 5) {
        packetTypes = (short*)packetTypesGen5;
        payloadLengths = (short*)payloadLengthsGen5;
        preconstructedPayloads = (char**)preconstructedPayloadsGen5;
    }
    else {
        if (encryptedControlStream) {
            packetTypes = (short*)packetTypesGen7Enc;
        }
        else {
            packetTypes = (short*)packetTypesGen7;
        }
        payloadLengths = (short*)payloadLengthsGen7;
        preconstructedPayloads = (char**)preconstructedPayloadsGen7;
    }

    idrFrameRequired = false;
    lastGoodFrame = 0;
    lastSeenFrame = 0;
    lossCountSinceLastReport = 0;
    disconnectPending = false;
    intervalGoodFrameCount = 0;
    intervalTotalFrameCount = 0;
    intervalStartTimeMs = 0;
    lastIntervalLossPercentage = 0;
    lastConnectionStatusUpdate = CONN_STATUS_OKAY;
    currentEnetSequenceNumber = 0;
    usePeriodicPing = APP_VERSION_AT_LEAST(7, 1, 415);
    cipherContext = EVP_CIPHER_CTX_new();

    return 0;
}

void freeFrameInvalidationList(PLINKED_BLOCKING_QUEUE_ENTRY entry) {
    PLINKED_BLOCKING_QUEUE_ENTRY nextEntry;

    while (entry != NULL) {
        nextEntry = entry->flink;
        free(entry->data);
        entry = nextEntry;
    }
}

// Cleans up control stream
void destroyControlStream(void) {
    LC_ASSERT(stopping);
    EVP_CIPHER_CTX_free(cipherContext);
    PltCloseEvent(&invalidateRefFramesEvent);
    freeFrameInvalidationList(LbqDestroyLinkedBlockingQueue(&invalidReferenceFrameTuples));
    PltDeleteMutex(&enetMutex);
}

int getNextFrameInvalidationTuple(PQUEUED_FRAME_INVALIDATION_TUPLE* qfit) {
    int err = LbqPollQueueElement(&invalidReferenceFrameTuples, (void**)qfit);
    return (err == LBQ_SUCCESS);
}

void queueFrameInvalidationTuple(int startFrame, int endFrame) {
    LC_ASSERT(startFrame <= endFrame);
    
    if (isReferenceFrameInvalidationEnabled()) {
        PQUEUED_FRAME_INVALIDATION_TUPLE qfit;
        qfit = malloc(sizeof(*qfit));
        if (qfit != NULL) {
            qfit->startFrame = startFrame;
            qfit->endFrame = endFrame;
            if (LbqOfferQueueItem(&invalidReferenceFrameTuples, qfit, &qfit->entry) == LBQ_BOUND_EXCEEDED) {
                // Too many invalidation tuples, so we need an IDR frame now
                free(qfit);
                idrFrameRequired = true;
            }
        }
        else {
            idrFrameRequired = true;
        }
    }
    else {
        idrFrameRequired = true;
    }

    PltSetEvent(&invalidateRefFramesEvent);
}

// Request an IDR frame on demand by the decoder
void requestIdrOnDemand(void) {
    idrFrameRequired = true;
    PltSetEvent(&invalidateRefFramesEvent);
}

// Invalidate reference frames lost by the network
void connectionDetectedFrameLoss(int startFrame, int endFrame) {
    queueFrameInvalidationTuple(startFrame, endFrame);
}

// When we receive a frame, update the number of our current frame
void connectionReceivedCompleteFrame(int frameIndex) {
    lastGoodFrame = frameIndex;
    intervalGoodFrameCount++;
}

void connectionSawFrame(int frameIndex) {
    LC_ASSERT(!isBefore16(frameIndex, lastSeenFrame));

    uint64_t now = PltGetMillis();
    if (now - intervalStartTimeMs >= CONN_STATUS_SAMPLE_PERIOD) {
        if (intervalTotalFrameCount != 0) {
            // Notify the client of connection status changes based on frame loss rate
            int frameLossPercent = 100 - (intervalGoodFrameCount * 100) / intervalTotalFrameCount;
            if (lastConnectionStatusUpdate != CONN_STATUS_POOR &&
                    (frameLossPercent >= CONN_IMMEDIATE_POOR_LOSS_RATE ||
                     (frameLossPercent >= CONN_CONSECUTIVE_POOR_LOSS_RATE && lastIntervalLossPercentage >= CONN_CONSECUTIVE_POOR_LOSS_RATE))) {
                // We require 2 consecutive intervals above CONN_CONSECUTIVE_POOR_LOSS_RATE or a single
                // interval above CONN_IMMEDIATE_POOR_LOSS_RATE to notify of a poor connection.
                ListenerCallbacks.connectionStatusUpdate(CONN_STATUS_POOR);
                lastConnectionStatusUpdate = CONN_STATUS_POOR;
            }
            else if (frameLossPercent <= CONN_OKAY_LOSS_RATE && lastConnectionStatusUpdate != CONN_STATUS_OKAY) {
                ListenerCallbacks.connectionStatusUpdate(CONN_STATUS_OKAY);
                lastConnectionStatusUpdate = CONN_STATUS_OKAY;
            }

            lastIntervalLossPercentage = frameLossPercent;
        }

        // Reset interval
        intervalStartTimeMs = now;
        intervalGoodFrameCount = intervalTotalFrameCount = 0;
    }

    intervalTotalFrameCount += frameIndex - lastSeenFrame;
    lastSeenFrame = frameIndex;
}

// When we lose packets, update our packet loss count
void connectionLostPackets(int lastReceivedPacket, int nextReceivedPacket) {
    lossCountSinceLastReport += (nextReceivedPacket - lastReceivedPacket) - 1;
}

// Reads an NV control stream packet from the TCP connection
static PNVCTL_TCP_PACKET_HEADER readNvctlPacketTcp(void) {
    NVCTL_TCP_PACKET_HEADER staticHeader;
    PNVCTL_TCP_PACKET_HEADER fullPacket;
    SOCK_RET err;

    err = recv(ctlSock, (char*)&staticHeader, sizeof(staticHeader), 0);
    if (err != sizeof(staticHeader)) {
        return NULL;
    }

    fullPacket = (PNVCTL_TCP_PACKET_HEADER)malloc(staticHeader.payloadLength + sizeof(staticHeader));
    if (fullPacket == NULL) {
        return NULL;
    }

    memcpy(fullPacket, &staticHeader, sizeof(staticHeader));
    if (staticHeader.payloadLength != 0) {
        err = recv(ctlSock, (char*)(fullPacket + 1), staticHeader.payloadLength, 0);
        if (err != staticHeader.payloadLength) {
            free(fullPacket);
            return NULL;
        }
    }

    return fullPacket;
}

static bool encryptControlMessage(PNVCTL_ENCRYPTED_PACKET_HEADER encPacket, PNVCTL_ENET_PACKET_HEADER_V2 packet) {
    bool ret = false;
    int len;
    unsigned char iv[16];

    // This is a truncating cast, but it's what Nvidia does, so we have to mimic it.
    memset(iv, 0, sizeof(iv));
    iv[0] = (unsigned char)encPacket->seq;

    if (EVP_EncryptInit_ex(cipherContext, EVP_aes_128_gcm(), NULL, NULL, NULL) != 1) {
        goto gcm_cleanup;
    }

    if (EVP_CIPHER_CTX_ctrl(cipherContext, EVP_CTRL_GCM_SET_IVLEN, 16, NULL) != 1) {
        goto gcm_cleanup;
    }

    if (EVP_EncryptInit_ex(cipherContext, NULL, NULL,
                           (const unsigned char*)StreamConfig.remoteInputAesKey, iv) != 1) {
        goto gcm_cleanup;
    }

    // Encrypt into the space after the encrypted header and GCM tag
    int encryptedSize = sizeof(*packet) + packet->payloadLength;
#ifdef BIGENDIAN
    packet->type = __bswap16(packet->type);
    packet->payloadLength = __bswap16(packet->payloadLength);
#endif
    if (EVP_EncryptUpdate(cipherContext, ((unsigned char*)(encPacket + 1)) + AES_GCM_TAG_LENGTH,
                          &encryptedSize, (const unsigned char*)packet, encryptedSize) != 1) {
        goto gcm_cleanup;
    }

    // GCM encryption won't ever fill ciphertext here but we have to call it anyway
    if (EVP_EncryptFinal_ex(cipherContext, ((unsigned char*)(encPacket + 1)), &len) != 1) {
        goto gcm_cleanup;
    }
    LC_ASSERT(len == 0);

    // Read the tag into the space after the encrypted header
    if (EVP_CIPHER_CTX_ctrl(cipherContext, EVP_CTRL_GCM_GET_TAG, 16, (unsigned char*)(encPacket + 1)) != 1) {
        ret = -1;
        goto gcm_cleanup;
    }

    ret = true;

gcm_cleanup:
    EVP_CIPHER_CTX_reset(cipherContext);
    return ret;
}

// Caller must free() *packet on success!!!
static bool decryptControlMessageToV1(PNVCTL_ENCRYPTED_PACKET_HEADER encPacket, PNVCTL_ENET_PACKET_HEADER_V1* packet, int* packetLength) {
    bool ret = false;
    int len;
    unsigned char iv[16];

    *packet = NULL;

#ifdef BIGENDIAN
    encPacket->length = __bswap16(encPacket->length);
    encPacket->seq = __bswap32(encPacket->seq);
#endif

    // It must be an encrypted packet to begin with
    LC_ASSERT(encPacket->encryptedHeaderType == 0x0001);

    // Check length first so we don't underflow
    if (encPacket->length < sizeof(encPacket->seq) + AES_GCM_TAG_LENGTH + sizeof(NVCTL_ENET_PACKET_HEADER_V2)) {
        Limelog("Received runt packet (%d). Unable to decrypt.\n", encPacket->length);
        return false;
    }

    // This is a truncating cast, but it's what Nvidia does, so we have to mimic it.
    memset(iv, 0, sizeof(iv));
    iv[0] = (unsigned char)encPacket->seq;

    if (EVP_DecryptInit_ex(cipherContext, EVP_aes_128_gcm(), NULL, NULL, NULL) != 1) {
        goto gcm_cleanup;
    }

    if (EVP_CIPHER_CTX_ctrl(cipherContext, EVP_CTRL_GCM_SET_IVLEN, 16, NULL) != 1) {
        goto gcm_cleanup;
    }

    if (EVP_DecryptInit_ex(cipherContext, NULL, NULL,
                           (const unsigned char*)StreamConfig.remoteInputAesKey, iv) != 1) {
        goto gcm_cleanup;
    }

    int plaintextLength = encPacket->length - sizeof(encPacket->seq) - AES_GCM_TAG_LENGTH;
    *packet = malloc(plaintextLength);
    if (*packet == NULL) {
        goto gcm_cleanup;
    }

    // Decrypt into the packet we allocated
    if (EVP_DecryptUpdate(cipherContext, (unsigned char*)*packet, &plaintextLength,
                          ((unsigned char*)(encPacket + 1)) + AES_GCM_TAG_LENGTH, plaintextLength) != 1) {
        goto gcm_cleanup;
    }

    // Set the GCM tag before calling EVP_DecryptFinal_ex()
    if (EVP_CIPHER_CTX_ctrl(cipherContext, EVP_CTRL_GCM_SET_TAG, 16, (unsigned char*)(encPacket + 1)) != 1) {
        ret = -1;
        goto gcm_cleanup;
    }

    // GCM will never have additional plaintext here, but we need to call it to
    // ensure that the GCM authentication tag is correct for this data.
    if (EVP_DecryptFinal_ex(cipherContext, (unsigned char*)*packet, &len) != 1) {
        goto gcm_cleanup;
    }
    LC_ASSERT(len == 0);

    // Now we do an in-place V2 to V1 header conversion, so our existing parsing code doesn't have to change.
    // All we need to do is eliminate the new length field in V2 by shifting everything by 2 bytes.
    memmove(((unsigned char*)*packet) + 2, ((unsigned char*)*packet) + 4, plaintextLength - 4);
    *packetLength = plaintextLength - 2;

    ret = true;

gcm_cleanup:
    EVP_CIPHER_CTX_reset(cipherContext);

    if (!ret && *packet) {
        free(*packet);
        *packet = NULL;
    }

    return ret;
}

static bool sendMessageEnet(short ptype, short paylen, const void* payload) {
    ENetPacket* enetPacket;
    int err;

    LC_ASSERT(AppVersionQuad[0] >= 5);

    if (encryptedControlStream) {
        PNVCTL_ENCRYPTED_PACKET_HEADER encPacket;
        PNVCTL_ENET_PACKET_HEADER_V2 packet;
        char tempBuffer[256];

        enetPacket = enet_packet_create(NULL,
                                        sizeof(*encPacket) + AES_GCM_TAG_LENGTH + sizeof(*packet) + paylen,
                                        ENET_PACKET_FLAG_RELIABLE);
        if (enetPacket == NULL) {
            return false;
        }

        // We (ab)use the enetMutex to protect currentEnetSequenceNumber and the cipherContext
        // used inside encryptControlMessage().
        PltLockMutex(&enetMutex);

        encPacket = (PNVCTL_ENCRYPTED_PACKET_HEADER)enetPacket->data;
        encPacket->encryptedHeaderType = 0x0001;
        encPacket->length = sizeof(encPacket->seq) + AES_GCM_TAG_LENGTH + sizeof(*packet) + paylen;
        encPacket->seq = currentEnetSequenceNumber++;
#ifdef BIGENDIAN
        encPacket->encryptedHeaderType = __bswap16(encPacket->encryptedHeaderType);
        encPacket->length = __bswap16(encPacket->length);
        encPacket->seq = __bswap32(encPacket->seq);
#endif

        // Construct the plaintext data for encryption
        LC_ASSERT(sizeof(*packet) + paylen < sizeof(tempBuffer));
        packet = (PNVCTL_ENET_PACKET_HEADER_V2)tempBuffer;
        packet->type = ptype;
        packet->payloadLength = paylen;
        memcpy(&packet[1], payload, paylen);

        // Encrypt the data into the final packet
        if (!encryptControlMessage(encPacket, packet)) {
            Limelog("Failed to encrypt control stream message\n");
            enet_packet_destroy(enetPacket);
            PltUnlockMutex(&enetMutex);
            return false;
        }

        PltUnlockMutex(&enetMutex);
    }
    else {
        PNVCTL_ENET_PACKET_HEADER_V1 packet;
        enetPacket = enet_packet_create(NULL, sizeof(*packet) + paylen, ENET_PACKET_FLAG_RELIABLE);
        if (enetPacket == NULL) {
            return false;
        }

        packet = (PNVCTL_ENET_PACKET_HEADER_V1)enetPacket->data;
#ifdef BIGENDIAN
        packet->type = __bswap16(ptype);
#else
        packet->type = ptype;
#endif
        memcpy(&packet[1], payload, paylen);
    }

    PltLockMutex(&enetMutex);
    err = enet_peer_send(peer, 0, enetPacket);
    PltUnlockMutex(&enetMutex);
    if (err < 0) {
        Limelog("Failed to send ENet control packet\n");
        enet_packet_destroy(enetPacket);
        return false;
    }
    
    PltLockMutex(&enetMutex);
    enet_host_flush(client);
    PltUnlockMutex(&enetMutex);

    return true;
}

static bool sendMessageTcp(short ptype, short paylen, const void* payload) {
    PNVCTL_TCP_PACKET_HEADER packet;
    SOCK_RET err;

    LC_ASSERT(AppVersionQuad[0] < 5);

    packet = malloc(sizeof(*packet) + paylen);
    if (packet == NULL) {
        return false;
    }

#ifdef BIGENDIAN
    packet->type = __bswap16(ptype);
    packet->payloadLength = __bswap16(paylen);
#else
    packet->type = ptype;
    packet->payloadLength = paylen;
#endif
    memcpy(&packet[1], payload, paylen);

    err = send(ctlSock, (char*) packet, sizeof(*packet) + paylen, 0);
    free(packet);

    if (err != (SOCK_RET)(sizeof(*packet) + paylen)) {
        return false;
    }

    return true;
}

static bool sendMessageAndForget(short ptype, short paylen, const void* payload) {
    bool ret;

    // Unlike regular sockets, ENet sockets aren't safe to invoke from multiple
    // threads at once. We have to synchronize them with a lock.
    if (AppVersionQuad[0] >= 5) {
        ret = sendMessageEnet(ptype, paylen, payload);
    }
    else {
        ret = sendMessageTcp(ptype, paylen, payload);
    }

    return ret;
}

static bool sendMessageAndDiscardReply(short ptype, short paylen, const void* payload) {
    if (AppVersionQuad[0] >= 5) {
        if (!sendMessageEnet(ptype, paylen, payload)) {
            return false;
        }
    }
    else {
        PNVCTL_TCP_PACKET_HEADER reply;

        if (!sendMessageTcp(ptype, paylen, payload)) {
            return false;
        }

        // Discard the response
        reply = readNvctlPacketTcp();
        if (reply == NULL) {
            return false;
        }

        free(reply);
    }

    return true;
}

// This intercept function drops disconnect events to allow us to process
// pending receives first. It works around what appears to be a bug in ENet
// where pending disconnects can cause loss of unprocessed received data.
static int ignoreDisconnectIntercept(ENetHost* host, ENetEvent* event) {
    if (host->receivedDataLength == sizeof(ENetProtocolHeader) + sizeof(ENetProtocolDisconnect)) {
        ENetProtocolHeader* protoHeader = (ENetProtocolHeader*)host->receivedData;
        ENetProtocolDisconnect* disconnect = (ENetProtocolDisconnect*)(protoHeader + 1);

        if ((disconnect->header.command & ENET_PROTOCOL_COMMAND_MASK) == ENET_PROTOCOL_COMMAND_DISCONNECT) {
            Limelog("ENet disconnect event pending\n");
            disconnectPending = true;
            if (event) {
                event->type = ENET_EVENT_TYPE_NONE;
            }
            return 1;
        }
    }

    return 0;
}

static void controlReceiveThreadFunc(void* context) {
    int err;

    // This is only used for ENet
    if (AppVersionQuad[0] < 5) {
        return;
    }

    while (!PltIsThreadInterrupted(&controlReceiveThread)) {
        ENetEvent event;

        // Poll for new packets and process retransmissions
        PltLockMutex(&enetMutex);
        err = serviceEnetHost(client, &event, 0);
        PltUnlockMutex(&enetMutex);
        if (err == 0) {
            // Handle a pending disconnect after unsuccessfully polling
            // for new events to handle.
            if (disconnectPending) {
                PltLockMutex(&enetMutex);
                // Wait 100 ms for pending receives after a disconnect and
                // 1 second for the pending disconnect to be processed after
                // removing the intercept callback.
                err = serviceEnetHost(client, &event, client->intercept ? 100 : 1000);
                if (err == 0) {
                    if (client->intercept) {
                        // Now that no pending receive events remain, we can
                        // remove our intercept hook and allow the server's
                        // disconnect to be processed as expected. We will wait
                        // 1 second for this disconnect to be processed before
                        // we tear down the connection anyway.
                        client->intercept = NULL;
                        PltUnlockMutex(&enetMutex);
                        continue;
                    }
                    else {
                        // The 1 second timeout has expired with no disconnect event
                        // retransmission after the first notification. We can only
                        // assume the server died tragically, so go ahead and tear down.
                        PltUnlockMutex(&enetMutex);
                        Limelog("Disconnect event timeout expired\n");
                        ListenerCallbacks.connectionTerminated(-1);
                        return;
                    }
                }
                else {
                    PltUnlockMutex(&enetMutex);
                }
            }
            else {
                // No events ready - sleep for a short time
                //
                // NOTE: This sleep *directly* impacts the lowest possible retransmission
                // time for packets after a loss event. If we're busy sleeping here, we can't
                // retransmit a dropped packet, so we keep the sleep time to a minimum.
                PltSleepMsInterruptible(&controlReceiveThread, 10);
                continue;
            }
        }

        if (err < 0) {
            Limelog("Control stream connection failed: %d\n", err);
            ListenerCallbacks.connectionTerminated(err);
            return;
        }

        if (event.type == ENET_EVENT_TYPE_RECEIVE) {
            PNVCTL_ENET_PACKET_HEADER_V1 ctlHdr;
            int packetLength;

            if (event.packet->dataLength < sizeof(*ctlHdr)) {
                Limelog("Discarding runt control packet: %d < %d\n", event.packet->dataLength, (int)sizeof(*ctlHdr));
                enet_packet_destroy(event.packet);
                continue;
            }

            ctlHdr = (PNVCTL_ENET_PACKET_HEADER_V1)event.packet->data;
#ifdef BIGENDIAN
            ctlHdr->type = __bswap16(ctlHdr->type);
#endif

            if (encryptedControlStream) {
                // V2 headers can be interpreted as V1 headers for the purpose of examining type,
                // so this check is safe.
                if (ctlHdr->type == 0x0001) {
                    if (event.packet->dataLength < sizeof(NVCTL_ENCRYPTED_PACKET_HEADER)) {
                        Limelog("Discarding runt encrypted control packet: %d < %d\n", event.packet->dataLength, (int)sizeof(NVCTL_ENCRYPTED_PACKET_HEADER));
                        enet_packet_destroy(event.packet);
                        continue;
                    }

                    // We (ab)use this lock to protect the cryptoContext too
                    PltLockMutex(&enetMutex);
                    ctlHdr = NULL;
                    if (!decryptControlMessageToV1((PNVCTL_ENCRYPTED_PACKET_HEADER)event.packet->data, &ctlHdr, &packetLength)) {
                        PltUnlockMutex(&enetMutex);
                        Limelog("Failed to decrypt control packet of size %d\n", event.packet->dataLength);
                        enet_packet_destroy(event.packet);
                        continue;
                    }
                    PltUnlockMutex(&enetMutex);
                }
                else {
                    // What do we do here???
                    LC_ASSERT(false);
                    packetLength = event.packet->dataLength;
                }
            }
            else {
                // Take ownership of the packet data directly for the non-encrypted case
                ctlHdr = (PNVCTL_ENET_PACKET_HEADER_V1)event.packet->data;
                packetLength = event.packet->dataLength;
                event.packet->data = NULL;
            }

            // We're done with the packet struct
            enet_packet_destroy(event.packet);

            // All below codepaths must free ctlHdr!!!

            if (ctlHdr->type == packetTypes[IDX_RUMBLE_DATA]) {
                BYTE_BUFFER bb;

                BbInitializeWrappedBuffer(&bb, (char*)ctlHdr, sizeof(*ctlHdr), packetLength - sizeof(*ctlHdr), BYTE_ORDER_LITTLE);
                BbAdvanceBuffer(&bb, 4);

                uint16_t controllerNumber;
                uint16_t lowFreqRumble;
                uint16_t highFreqRumble;

                BbGet16(&bb, &controllerNumber);
                BbGet16(&bb, &lowFreqRumble);
                BbGet16(&bb, &highFreqRumble);

                ListenerCallbacks.rumble(controllerNumber, lowFreqRumble, highFreqRumble);
            }
            else if (ctlHdr->type == packetTypes[IDX_TERMINATION]) {
                BYTE_BUFFER bb;


                uint32_t terminationErrorCode;

                if (packetLength >= 6) {
                    // This is the extended termination message which contains a full HRESULT
                    BbInitializeWrappedBuffer(&bb, (char*)ctlHdr, sizeof(*ctlHdr), packetLength - sizeof(*ctlHdr), BYTE_ORDER_BIG);
                    BbGet32(&bb, &terminationErrorCode);

                    Limelog("Server notified termination reason: 0x%08x\n", terminationErrorCode);

                    // NVST_DISCONN_SERVER_TERMINATED_CLOSED is the expected graceful termination error
                    if (terminationErrorCode == 0x80030023) {
                        if (lastSeenFrame != 0) {
                            // Pass error code 0 to notify the client that this was not an error
                            terminationErrorCode = ML_ERROR_GRACEFUL_TERMINATION;
                        }
                        else {
                            // We never saw a frame, so this is probably an error that caused
                            // NvStreamer to terminate prior to sending any frames.
                            terminationErrorCode = ML_ERROR_UNEXPECTED_EARLY_TERMINATION;
                        }
                    }
                    // NVST_DISCONN_SERVER_VFP_PROTECTED_CONTENT means it failed due to protected content on screen
                    else if (terminationErrorCode == 0x800e9302) {
                        terminationErrorCode = ML_ERROR_PROTECTED_CONTENT;
                    }
                }
                else {
                    uint16_t terminationReason;

                    // This is the short termination message
                    BbInitializeWrappedBuffer(&bb, (char*)ctlHdr, sizeof(*ctlHdr), packetLength - sizeof(*ctlHdr), BYTE_ORDER_LITTLE);
                    BbGet16(&bb, &terminationReason);

                    Limelog("Server notified termination reason: 0x%04x\n", terminationReason);

                    // SERVER_TERMINATED_INTENDED
                    if (terminationReason == 0x0100) {
                        if (lastSeenFrame != 0) {
                            // Pass error code 0 to notify the client that this was not an error
                            terminationErrorCode = ML_ERROR_GRACEFUL_TERMINATION;
                        }
                        else {
                            // We never saw a frame, so this is probably an error that caused
                            // NvStreamer to terminate prior to sending any frames.
                            terminationErrorCode = ML_ERROR_UNEXPECTED_EARLY_TERMINATION;
                        }
                    }
                    else {
                        // Otherwise pass the reason unmodified
                        terminationErrorCode = terminationReason;
                    }
                }

                // We used to wait for a ENET_EVENT_TYPE_DISCONNECT event, but since
                // GFE 3.20.3.63 we don't get one for 10 seconds after we first get
                // this termination message. The termination message should be reliable
                // enough to end the stream now, rather than waiting for an explicit
                // disconnect.
                ListenerCallbacks.connectionTerminated((int)terminationErrorCode);
                free(ctlHdr);
                return;
            }

            free(ctlHdr);
        }
        else if (event.type == ENET_EVENT_TYPE_DISCONNECT) {
            Limelog("Control stream received unexpected disconnect event\n");
            ListenerCallbacks.connectionTerminated(-1);
            return;
        }
    }
}

static void lossStatsThreadFunc(void* context) {
    BYTE_BUFFER byteBuffer;

    if (usePeriodicPing) {
        char periodicPingPayload[8];

        BbInitializeWrappedBuffer(&byteBuffer, periodicPingPayload, 0, sizeof(periodicPingPayload), BYTE_ORDER_LITTLE);
        BbPut16(&byteBuffer, 4); // Length of payload
        BbPut32(&byteBuffer, 0); // Timestamp?

        while (!PltIsThreadInterrupted(&lossStatsThread)) {
            // Send the message (and don't expect a response)
            if (!sendMessageAndForget(0x0200, sizeof(periodicPingPayload), periodicPingPayload)) {
                Limelog("Loss Stats: Transaction failed: %d\n", (int)LastSocketError());
                ListenerCallbacks.connectionTerminated(LastSocketFail());
                return;
            }

            // Wait a bit
            PltSleepMsInterruptible(&lossStatsThread, PERIODIC_PING_INTERVAL_MS);
        }
    }
    else {
        char* lossStatsPayload;

        lossStatsPayload = malloc(payloadLengths[IDX_LOSS_STATS]);
        if (lossStatsPayload == NULL) {
            Limelog("Loss Stats: malloc() failed\n");
            ListenerCallbacks.connectionTerminated(-1);
            return;
        }

        while (!PltIsThreadInterrupted(&lossStatsThread)) {
            // Construct the payload
            BbInitializeWrappedBuffer(&byteBuffer, lossStatsPayload, 0, payloadLengths[IDX_LOSS_STATS], BYTE_ORDER_LITTLE);
            BbPut32(&byteBuffer, lossCountSinceLastReport);
            BbPut32(&byteBuffer, LOSS_REPORT_INTERVAL_MS);
            BbPut32(&byteBuffer, 1000);
            BbPut64(&byteBuffer, lastGoodFrame);
            BbPut32(&byteBuffer, 0);
            BbPut32(&byteBuffer, 0);
            BbPut32(&byteBuffer, 0x14);

            // Send the message (and don't expect a response)
            if (!sendMessageAndForget(packetTypes[IDX_LOSS_STATS],
                payloadLengths[IDX_LOSS_STATS], lossStatsPayload)) {
                free(lossStatsPayload);
                Limelog("Loss Stats: Transaction failed: %d\n", (int)LastSocketError());
                ListenerCallbacks.connectionTerminated(LastSocketFail());
                return;
            }

            // Clear the transient state
            lossCountSinceLastReport = 0;

            // Wait a bit
            PltSleepMsInterruptible(&lossStatsThread, LOSS_REPORT_INTERVAL_MS);
        }

        free(lossStatsPayload);
    }
}

static void requestIdrFrame(void) {
    int64_t payload[3];

    if (AppVersionQuad[0] >= 5) {
        // Form the payload
        if (lastSeenFrame < 0x20) {
            payload[0] = 0;
            payload[1] = lastSeenFrame;
        }
        else {
            payload[0] = lastSeenFrame - 0x20;
            payload[1] = lastSeenFrame;
        }

        payload[2] = 0;

#ifdef BIGENDIAN
        payload[0] = __bswap64(payload[0]);
        payload[1] = __bswap64(payload[1]);
#endif

        // Send the reference frame invalidation request and read the response
        if (!sendMessageAndDiscardReply(packetTypes[IDX_INVALIDATE_REF_FRAMES],
            payloadLengths[IDX_INVALIDATE_REF_FRAMES], payload)) {
            Limelog("Request IDR Frame: Transaction failed: %d\n", (int)LastSocketError());
            ListenerCallbacks.connectionTerminated(LastSocketFail());
            return;
        }
    }
    else {
        // Send IDR frame request and read the response
        if (!sendMessageAndDiscardReply(packetTypes[IDX_REQUEST_IDR_FRAME],
            payloadLengths[IDX_REQUEST_IDR_FRAME], preconstructedPayloads[IDX_REQUEST_IDR_FRAME])) {
            Limelog("Request IDR Frame: Transaction failed: %d\n", (int)LastSocketError());
            ListenerCallbacks.connectionTerminated(LastSocketFail());
            return;
        }
    }

    Limelog("IDR frame request sent\n");
}

static void requestInvalidateReferenceFrames(void) {
    int64_t payload[3];
    PQUEUED_FRAME_INVALIDATION_TUPLE qfit;

    LC_ASSERT(isReferenceFrameInvalidationEnabled());

    if (!getNextFrameInvalidationTuple(&qfit)) {
        return;
    }

    LC_ASSERT(qfit->startFrame <= qfit->endFrame);

    payload[0] = qfit->startFrame;
    payload[1] = qfit->endFrame;
    payload[2] = 0;

    // Aggregate all lost frames into one range
    do {
        LC_ASSERT(qfit->endFrame >= payload[1]);
        payload[1] = qfit->endFrame;
        free(qfit);
    } while (getNextFrameInvalidationTuple(&qfit));

#ifdef BIGENDIAN
    payload[0] = __bswap64(payload[0]);
    payload[1] = __bswap64(payload[1]);
#endif

    // Send the reference frame invalidation request and read the response
    if (!sendMessageAndDiscardReply(packetTypes[IDX_INVALIDATE_REF_FRAMES],
        payloadLengths[IDX_INVALIDATE_REF_FRAMES], payload)) {
        Limelog("Request Invaldiate Reference Frames: Transaction failed: %d\n", (int)LastSocketError());
        ListenerCallbacks.connectionTerminated(LastSocketFail());
        return;
    }

    Limelog("Invalidate reference frame request sent (%d to %d)\n", (int)payload[0], (int)payload[1]);
}

static void invalidateRefFramesFunc(void* context) {
    while (!PltIsThreadInterrupted(&invalidateRefFramesThread)) {
        // Wait for a request to invalidate reference frames
        PltWaitForEvent(&invalidateRefFramesEvent);
        PltClearEvent(&invalidateRefFramesEvent);
        
        // Bail if we've been shutdown
        if (stopping) {
            break;
        }

        // Sometimes we absolutely need an IDR frame
        if (idrFrameRequired) {
            // Empty invalidate reference frames tuples
            PQUEUED_FRAME_INVALIDATION_TUPLE qfit;
            while (getNextFrameInvalidationTuple(&qfit)) {
                free(qfit);
            }

            // Send an IDR frame request
            idrFrameRequired = false;
            requestIdrFrame();
        }
        else {
            // Otherwise invalidate reference frames
            requestInvalidateReferenceFrames();
        }
    }
}

// Stops the control stream
int stopControlStream(void) {
    stopping = true;
    LbqSignalQueueShutdown(&invalidReferenceFrameTuples);
    PltSetEvent(&invalidateRefFramesEvent);

    // This must be set to stop in a timely manner
    LC_ASSERT(ConnectionInterrupted);

    if (ctlSock != INVALID_SOCKET) {
        shutdownTcpSocket(ctlSock);
    }
    
    PltInterruptThread(&lossStatsThread);
    PltInterruptThread(&invalidateRefFramesThread);
    PltInterruptThread(&controlReceiveThread);

    PltJoinThread(&lossStatsThread);
    PltJoinThread(&invalidateRefFramesThread);
    PltJoinThread(&controlReceiveThread);

    PltCloseThread(&lossStatsThread);
    PltCloseThread(&invalidateRefFramesThread);
    PltCloseThread(&controlReceiveThread);

    if (peer != NULL) {
        // We use enet_peer_disconnect_now() so the host knows immediately
        // of our termination and can cleanup properly for reconnection.
        enet_peer_disconnect_now(peer, 0);
        peer = NULL;
    }
    if (client != NULL) {
        enet_host_destroy(client);
        client = NULL;
    }
    
    if (ctlSock != INVALID_SOCKET) {
        closeSocket(ctlSock);
        ctlSock = INVALID_SOCKET;
    }

    return 0;
}

// Called by the input stream to send a packet for Gen 5+ servers
int sendInputPacketOnControlStream(unsigned char* data, int length) {
    LC_ASSERT(AppVersionQuad[0] >= 5);

    // Send the input data (no reply expected)
    if (sendMessageAndForget(packetTypes[IDX_INPUT_DATA], length, data) == 0) {
        return -1;
    }

    return 0;
}

// Starts the control stream
int startControlStream(void) {
    int err;

    if (AppVersionQuad[0] >= 5) {
        ENetAddress address;
        ENetEvent event;
        
        enet_address_set_address(&address, (struct sockaddr *)&RemoteAddr, RemoteAddrLen);
        enet_address_set_port(&address, 47999);

        // Create a client that can use 1 outgoing connection and 1 channel
        client = enet_host_create(address.address.ss_family, NULL, 1, 1, 0, 0);
        if (client == NULL) {
            stopping = true;
            return -1;
        }

        client->intercept = ignoreDisconnectIntercept;

        // Connect to the host
        peer = enet_host_connect(client, &address, 1, 0);
        if (peer == NULL) {
            stopping = true;
            enet_host_destroy(client);
            client = NULL;
            return -1;
        }

        // Wait for the connect to complete
        if (serviceEnetHost(client, &event, CONTROL_STREAM_TIMEOUT_SEC * 1000) <= 0 ||
            event.type != ENET_EVENT_TYPE_CONNECT) {
            Limelog("Failed to connect to UDP port 47999\n");
            stopping = true;
            enet_peer_reset(peer);
            peer = NULL;
            enet_host_destroy(client);
            client = NULL;
            return ETIMEDOUT;
        }

        // Ensure the connect verify ACK is sent immediately
        enet_host_flush(client);
        
        // Set the max peer timeout to 10 seconds
        enet_peer_timeout(peer, ENET_PEER_TIMEOUT_LIMIT, ENET_PEER_TIMEOUT_MINIMUM, 10000);
    }
    else {
        ctlSock = connectTcpSocket(&RemoteAddr, RemoteAddrLen,
            47995, CONTROL_STREAM_TIMEOUT_SEC);
        if (ctlSock == INVALID_SOCKET) {
            stopping = true;
            return LastSocketFail();
        }

        enableNoDelay(ctlSock);
    }

    err = PltCreateThread("ControlRecv", controlReceiveThreadFunc, NULL, &controlReceiveThread);
    if (err != 0) {
        stopping = true;
        if (ctlSock != INVALID_SOCKET) {
            closeSocket(ctlSock);
            ctlSock = INVALID_SOCKET;
        }
        else {
            enet_peer_disconnect_now(peer, 0);
            peer = NULL;
            enet_host_destroy(client);
            client = NULL;
        }
        return err;
    }

    // Send START A
    if (!sendMessageAndDiscardReply(packetTypes[IDX_START_A],
        payloadLengths[IDX_START_A],
        preconstructedPayloads[IDX_START_A])) {
        Limelog("Start A failed: %d\n", (int)LastSocketError());
        err = LastSocketFail();
        stopping = true;

        if (ctlSock != INVALID_SOCKET) {
            shutdownTcpSocket(ctlSock);
        }
        else {
            ConnectionInterrupted = true;
        }

        PltInterruptThread(&controlReceiveThread);
        PltJoinThread(&controlReceiveThread);
        PltCloseThread(&controlReceiveThread);

        if (ctlSock != INVALID_SOCKET) {
            closeSocket(ctlSock);
            ctlSock = INVALID_SOCKET;
        }
        else {
            enet_peer_disconnect_now(peer, 0);
            peer = NULL;
            enet_host_destroy(client);
            client = NULL;
        }
        return err;
    }

    // Send START B
    if (!sendMessageAndDiscardReply(packetTypes[IDX_START_B],
        payloadLengths[IDX_START_B],
        preconstructedPayloads[IDX_START_B])) {
        Limelog("Start B failed: %d\n", (int)LastSocketError());
        err = LastSocketFail();
        stopping = true;

        if (ctlSock != INVALID_SOCKET) {
            shutdownTcpSocket(ctlSock);
        }
        else {
            ConnectionInterrupted = true;
        }

        PltInterruptThread(&controlReceiveThread);
        PltJoinThread(&controlReceiveThread);
        PltCloseThread(&controlReceiveThread);

        if (ctlSock != INVALID_SOCKET) {
            closeSocket(ctlSock);
            ctlSock = INVALID_SOCKET;
        }
        else {
            enet_peer_disconnect_now(peer, 0);
            peer = NULL;
            enet_host_destroy(client);
            client = NULL;
        }
        return err;
    }

    err = PltCreateThread("LossStats", lossStatsThreadFunc, NULL, &lossStatsThread);
    if (err != 0) {
        stopping = true;

        if (ctlSock != INVALID_SOCKET) {
            shutdownTcpSocket(ctlSock);
        }
        else {
            ConnectionInterrupted = true;
        }

        PltInterruptThread(&controlReceiveThread);
        PltJoinThread(&controlReceiveThread);
        PltCloseThread(&controlReceiveThread);

        if (ctlSock != INVALID_SOCKET) {
            closeSocket(ctlSock);
            ctlSock = INVALID_SOCKET;
        }
        else {
            enet_peer_disconnect_now(peer, 0);
            peer = NULL;
            enet_host_destroy(client);
            client = NULL;
        }
        return err;
    }

    err = PltCreateThread("InvRefFrames", invalidateRefFramesFunc, NULL, &invalidateRefFramesThread);
    if (err != 0) {
        stopping = true;

        if (ctlSock != INVALID_SOCKET) {
            shutdownTcpSocket(ctlSock);
        }
        else {
            ConnectionInterrupted = true;
        }

        PltInterruptThread(&lossStatsThread);
        PltJoinThread(&lossStatsThread);
        PltCloseThread(&lossStatsThread);

        PltInterruptThread(&controlReceiveThread);
        PltJoinThread(&controlReceiveThread);
        PltCloseThread(&controlReceiveThread);

        if (ctlSock != INVALID_SOCKET) {
            closeSocket(ctlSock);
            ctlSock = INVALID_SOCKET;
        }
        else {
            enet_peer_disconnect_now(peer, 0);
            peer = NULL;
            enet_host_destroy(client);
            client = NULL;
        }

        return err;
    }

    return 0;
}
