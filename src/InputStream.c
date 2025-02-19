#include "Limelight-internal.h"
#include "PlatformSockets.h"
#include "PlatformThreads.h"
#include "LinkedBlockingQueue.h"
#include "Input.h"

#include <openssl/evp.h>

static SOCKET inputSock = INVALID_SOCKET;
static unsigned char currentAesIv[16];
static bool initialized;
static EVP_CIPHER_CTX* cipherContext;
static bool cipherInitialized;

static LINKED_BLOCKING_QUEUE packetQueue;
static PLT_THREAD inputSendThread;

#define MAX_INPUT_PACKET_SIZE 128
#define INPUT_STREAM_TIMEOUT_SEC 10

#define ROUND_TO_PKCS7_PADDED_LEN(x) ((((x) + 15) / 16) * 16)

// Contains input stream packets
typedef struct _PACKET_HOLDER {
    int packetLength;
    union {
        NV_KEYBOARD_PACKET keyboard;
        NV_REL_MOUSE_MOVE_PACKET mouseMoveRel;
        NV_ABS_MOUSE_MOVE_PACKET mouseMoveAbs;
        NV_MOUSE_BUTTON_PACKET mouseButton;
        NV_CONTROLLER_PACKET controller;
        NV_MULTI_CONTROLLER_PACKET multiController;
        NV_SCROLL_PACKET scroll;
        NV_HAPTICS_PACKET haptics;
    } packet;
    LINKED_BLOCKING_QUEUE_ENTRY entry;
} PACKET_HOLDER, *PPACKET_HOLDER;

#if OPENSSL_VERSION_NUMBER < 0x10100000L
#define EVP_CIPHER_CTX_reset(x) EVP_CIPHER_CTX_cleanup(x); EVP_CIPHER_CTX_init(x)
#endif

// Initializes the input stream
int initializeInputStream(void) {
    memcpy(currentAesIv, StreamConfig.remoteInputAesIv, sizeof(currentAesIv));
    
    // Initialized on first packet
    cipherInitialized = false;
    
    LbqInitializeLinkedBlockingQueue(&packetQueue, 30);

    return 0;
}

// Destroys and cleans up the input stream
void destroyInputStream(void) {
    PLINKED_BLOCKING_QUEUE_ENTRY entry, nextEntry;
    
    if (cipherInitialized) {
        EVP_CIPHER_CTX_free(cipherContext);
        cipherInitialized = false;
    }

    entry = LbqDestroyLinkedBlockingQueue(&packetQueue);

    while (entry != NULL) {
        nextEntry = entry->flink;

        // The entry is stored in the data buffer
        free(entry->data);

        entry = nextEntry;
    }
}

static int addPkcs7PaddingInPlace(unsigned char* plaintext, int plaintextLen) {
    int i;
    int paddedLength = ROUND_TO_PKCS7_PADDED_LEN(plaintextLen);
    unsigned char paddingByte = (unsigned char)(16 - (plaintextLen % 16));
    
    for (i = plaintextLen; i < paddedLength; i++) {
        plaintext[i] = paddingByte;
    }
    
    return paddedLength;
}

static int encryptData(const unsigned char* plaintext, int plaintextLen,
                       unsigned char* ciphertext, int* ciphertextLen) {
    int ret;
    int len;
    
    if (AppVersionQuad[0] >= 7) {
        if (!cipherInitialized) {
            if ((cipherContext = EVP_CIPHER_CTX_new()) == NULL) {
                return -1;
            }
            cipherInitialized = true;
        }

        // Gen 7 servers use 128-bit AES GCM
        if (EVP_EncryptInit_ex(cipherContext, EVP_aes_128_gcm(), NULL, NULL, NULL) != 1) {
            ret = -1;
            goto gcm_cleanup;
        }
        
        // Gen 7 servers uses 16 byte IVs
        if (EVP_CIPHER_CTX_ctrl(cipherContext, EVP_CTRL_GCM_SET_IVLEN, 16, NULL) != 1) {
            ret = -1;
            goto gcm_cleanup;
        }
        
        // Initialize again but now provide our key and current IV
        if (EVP_EncryptInit_ex(cipherContext, NULL, NULL,
                               (const unsigned char*)StreamConfig.remoteInputAesKey, currentAesIv) != 1) {
            ret = -1;
            goto gcm_cleanup;
        }
        
        // Encrypt into the caller's buffer, leaving room for the auth tag to be prepended
        if (EVP_EncryptUpdate(cipherContext, &ciphertext[16], ciphertextLen, plaintext, plaintextLen) != 1) {
            ret = -1;
            goto gcm_cleanup;
        }
        
        // GCM encryption won't ever fill ciphertext here but we have to call it anyway
        if (EVP_EncryptFinal_ex(cipherContext, ciphertext, &len) != 1) {
            ret = -1;
            goto gcm_cleanup;
        }
        LC_ASSERT(len == 0);
        
        // Read the tag into the caller's buffer
        if (EVP_CIPHER_CTX_ctrl(cipherContext, EVP_CTRL_GCM_GET_TAG, 16, ciphertext) != 1) {
            ret = -1;
            goto gcm_cleanup;
        }
        
        // Increment the ciphertextLen to account for the tag
        *ciphertextLen += 16;
        
        ret = 0;
        
    gcm_cleanup:
        EVP_CIPHER_CTX_reset(cipherContext);
    }
    else {
        unsigned char paddedData[MAX_INPUT_PACKET_SIZE];
        int paddedLength;
        
        if (!cipherInitialized) {
            if ((cipherContext = EVP_CIPHER_CTX_new()) == NULL) {
                ret = -1;
                goto cbc_cleanup;
            }
            cipherInitialized = true;

            // Prior to Gen 7, 128-bit AES CBC is used for encryption
            if (EVP_EncryptInit_ex(cipherContext, EVP_aes_128_cbc(), NULL,
                                   (const unsigned char*)StreamConfig.remoteInputAesKey, currentAesIv) != 1) {
                ret = -1;
                goto cbc_cleanup;
            }
        }
        
        // Pad the data to the required block length
        memcpy(paddedData, plaintext, plaintextLen);
        paddedLength = addPkcs7PaddingInPlace(paddedData, plaintextLen);
        
        if (EVP_EncryptUpdate(cipherContext, ciphertext, ciphertextLen, paddedData, paddedLength) != 1) {
            ret = -1;
            goto cbc_cleanup;
        }
        
        ret = 0;

    cbc_cleanup:
        // Nothing to do
        ;
    }
    
    return ret;
}

// Input thread proc
static void inputSendThreadProc(void* context) {
    SOCK_RET err;
    PPACKET_HOLDER holder;
    char encryptedBuffer[MAX_INPUT_PACKET_SIZE];
    uint32_t encryptedSize;
    bool encryptedControlStream = APP_VERSION_AT_LEAST(7, 1, 431);

    while (!PltIsThreadInterrupted(&inputSendThread)) {
        int encryptedLengthPrefix;

        err = LbqWaitForQueueElement(&packetQueue, (void**)&holder);
        if (err != LBQ_SUCCESS) {
            return;
        }

        // If it's a multi-controller packet we can do batching
        if (holder->packet.multiController.header.packetType == htonl(PACKET_TYPE_MULTI_CONTROLLER)) {
            PPACKET_HOLDER controllerBatchHolder;
            PNV_MULTI_CONTROLLER_PACKET origPkt;

            origPkt = &holder->packet.multiController;
            for (;;) {
                PNV_MULTI_CONTROLLER_PACKET newPkt;

                // Peek at the next packet
                if (LbqPeekQueueElement(&packetQueue, (void**)&controllerBatchHolder) != LBQ_SUCCESS) {
                    break;
                }

                // If it's not a controller packet, we're done
                if (controllerBatchHolder->packet.multiController.header.packetType != htonl(PACKET_TYPE_MULTI_CONTROLLER)) {
                    break;
                }

                // Check if it's able to be batched
                // NB: GFE does some discarding of gamepad packets received very soon after another.
                // Thus, this batching is needed for correctness in some cases, as GFE will inexplicably
                // drop *newer* packets in that scenario. The brokenness can be tested with consecutive
                // calls to LiSendMultiControllerEvent() with different values for analog sticks (max -> zero).
                newPkt = &controllerBatchHolder->packet.multiController;
                if (newPkt->buttonFlags != origPkt->buttonFlags ||
                    newPkt->controllerNumber != origPkt->controllerNumber ||
                    newPkt->activeGamepadMask != origPkt->activeGamepadMask) {
                    // Batching not allowed
                    break;
                }

                // Remove the batchable controller packet
                if (LbqPollQueueElement(&packetQueue, (void**)&controllerBatchHolder) != LBQ_SUCCESS) {
                    break;
                }

                // Update the original packet
                origPkt->leftTrigger = newPkt->leftTrigger;
                origPkt->rightTrigger = newPkt->rightTrigger;
                origPkt->leftStickX = newPkt->leftStickX;
                origPkt->leftStickY = newPkt->leftStickY;
                origPkt->rightStickX = newPkt->rightStickX;
                origPkt->rightStickY = newPkt->rightStickY;

                // Free the batched packet holder
                free(controllerBatchHolder);
            }
        }
        // If it's a relative mouse move packet, we can also do batching
        else if (holder->packet.mouseMoveRel.header.packetType == htonl(PACKET_TYPE_REL_MOUSE_MOVE)) {
            PPACKET_HOLDER mouseBatchHolder;
            int totalDeltaX = (short)htons(holder->packet.mouseMoveRel.deltaX);
            int totalDeltaY = (short)htons(holder->packet.mouseMoveRel.deltaY);

            for (;;) {
                int partialDeltaX;
                int partialDeltaY;

                // Peek at the next packet
                if (LbqPeekQueueElement(&packetQueue, (void**)&mouseBatchHolder) != LBQ_SUCCESS) {
                    break;
                }

                // If it's not a mouse move packet, we're done
                if (mouseBatchHolder->packet.mouseMoveRel.header.packetType != htonl(PACKET_TYPE_REL_MOUSE_MOVE)) {
                    break;
                }

                partialDeltaX = (short)htons(mouseBatchHolder->packet.mouseMoveRel.deltaX);
                partialDeltaY = (short)htons(mouseBatchHolder->packet.mouseMoveRel.deltaY);

                // Check for overflow
                if (partialDeltaX + totalDeltaX > INT16_MAX ||
                    partialDeltaX + totalDeltaX < INT16_MIN ||
                    partialDeltaY + totalDeltaY > INT16_MAX ||
                    partialDeltaY + totalDeltaY < INT16_MIN) {
                    // Total delta would overflow our 16-bit short
                    break;
                }

                // Remove the batchable mouse move packet
                if (LbqPollQueueElement(&packetQueue, (void**)&mouseBatchHolder) != LBQ_SUCCESS) {
                    break;
                }

                totalDeltaX += partialDeltaX;
                totalDeltaY += partialDeltaY;

                // Free the batched packet holder
                free(mouseBatchHolder);
            }

            // Update the original packet
            holder->packet.mouseMoveRel.deltaX = htons((short)totalDeltaX);
            holder->packet.mouseMoveRel.deltaY = htons((short)totalDeltaY);
        }
        // If it's an absolute mouse move packet, we should only send the latest
        else if (holder->packet.mouseMoveAbs.header.packetType == htonl(PACKET_TYPE_ABS_MOUSE_MOVE)) {
            for (;;) {
                PPACKET_HOLDER mouseBatchHolder;

                // Peek at the next packet
                if (LbqPeekQueueElement(&packetQueue, (void**)&mouseBatchHolder) != LBQ_SUCCESS) {
                    break;
                }

                // If it's not a mouse position packet, we're done
                if (mouseBatchHolder->packet.mouseMoveAbs.header.packetType != htonl(PACKET_TYPE_ABS_MOUSE_MOVE)) {
                    break;
                }

                // Remove the mouse position packet
                if (LbqPollQueueElement(&packetQueue, (void**)&mouseBatchHolder) != LBQ_SUCCESS) {
                    break;
                }

                // Replace the current packet with the new one
                free(holder);
                holder = mouseBatchHolder;
            }
        }

        // On GFE 3.22, the entire control stream is encrypted (and support for separate RI encrypted)
        // has been removed. We send the plaintext packet through and the control stream code will do
        // the encryption.
        if (encryptedControlStream) {
            err = (SOCK_RET)sendInputPacketOnControlStream((unsigned char*)&holder->packet, holder->packetLength);
            free(holder);
            if (err < 0) {
                Limelog("Input: sendInputPacketOnControlStream() failed: %d\n", (int) err);
                ListenerCallbacks.connectionTerminated(err);
                return;
            }
        }
        else {
            // Encrypt the message into the output buffer while leaving room for the length
            encryptedSize = sizeof(encryptedBuffer) - 4;
            err = encryptData((const unsigned char*)&holder->packet, holder->packetLength,
                (unsigned char*)&encryptedBuffer[4], (int*)&encryptedSize);
            free(holder);
            if (err != 0) {
                Limelog("Input: Encryption failed: %d\n", (int)err);
                ListenerCallbacks.connectionTerminated(err);
                return;
            }

            // Prepend the length to the message
            encryptedLengthPrefix = htonl(encryptedSize);
            memcpy(&encryptedBuffer[0], &encryptedLengthPrefix, 4);

            if (AppVersionQuad[0] < 5) {
                // Send the encrypted payload
                err = send(inputSock, (const char*) encryptedBuffer,
                    (int) (encryptedSize + sizeof(encryptedLengthPrefix)), 0);
                if (err <= 0) {
                    Limelog("Input: send() failed: %d\n", (int) LastSocketError());
                    ListenerCallbacks.connectionTerminated(LastSocketFail());
                    return;
                }
            }
            else {
                // For reasons that I can't understand, NVIDIA decides to use the last 16
                // bytes of ciphertext in the most recent game controller packet as the IV for
                // future encryption. I think it may be a buffer overrun on their end but we'll have
                // to mimic it to work correctly.
                if (AppVersionQuad[0] >= 7 && encryptedSize >= 16 + sizeof(currentAesIv)) {
                    memcpy(currentAesIv,
                           &encryptedBuffer[4 + encryptedSize - sizeof(currentAesIv)],
                           sizeof(currentAesIv));
                }

                err = (SOCK_RET)sendInputPacketOnControlStream((unsigned char*) encryptedBuffer,
                    (int) (encryptedSize + sizeof(encryptedLengthPrefix)));
                if (err < 0) {
                    Limelog("Input: sendInputPacketOnControlStream() failed: %d\n", (int) err);
                    ListenerCallbacks.connectionTerminated(err);
                    return;
                }
            }
        }
    }
}

// This function tells GFE that we support haptics and it should send rumble events to us
static int sendEnableHaptics(void) {
    PPACKET_HOLDER holder;
    int err;

    // Avoid sending this on earlier server versions, since they may terminate
    // the connection upon receiving an unexpected packet.
    if (AppVersionQuad[0] < 7 || (AppVersionQuad[0] == 7 && AppVersionQuad[1] < 1)) {
        return 0;
    }

    holder = malloc(sizeof(*holder));
    if (holder == NULL) {
        return -1;
    }

    holder->packetLength = sizeof(NV_HAPTICS_PACKET);
    holder->packet.haptics.header.packetType = htonl(PACKET_TYPE_HAPTICS);
    holder->packet.haptics.magicA = H_MAGIC_A;
    holder->packet.haptics.magicB = H_MAGIC_B;

    err = LbqOfferQueueItem(&packetQueue, holder, &holder->entry);
    if (err != LBQ_SUCCESS) {
        free(holder);
    }

    return err;
}

// Begin the input stream
int startInputStream(void) {
    int err;

    // After Gen 5, we send input on the control stream
    if (AppVersionQuad[0] < 5) {
        inputSock = connectTcpSocket(&RemoteAddr, RemoteAddrLen,
            35043, INPUT_STREAM_TIMEOUT_SEC);
        if (inputSock == INVALID_SOCKET) {
            return LastSocketFail();
        }

        enableNoDelay(inputSock);
    }

    err = PltCreateThread("InputSend", inputSendThreadProc, NULL, &inputSendThread);
    if (err != 0) {
        if (inputSock != INVALID_SOCKET) {
            closeSocket(inputSock);
            inputSock = INVALID_SOCKET;
        }
        return err;
    }

    // Allow input packets to be queued now
    initialized = true;

    // GFE will not send haptics events without this magic packet first
    sendEnableHaptics();

    return err;
}

// Stops the input stream
int stopInputStream(void) {
    // No more packets should be queued now
    initialized = false;

    // Signal the input send thread
    LbqSignalQueueShutdown(&packetQueue);
    PltInterruptThread(&inputSendThread);

    if (inputSock != INVALID_SOCKET) {
        shutdownTcpSocket(inputSock);
    }

    PltJoinThread(&inputSendThread);
    PltCloseThread(&inputSendThread);
    
    if (inputSock != INVALID_SOCKET) {
        closeSocket(inputSock);
        inputSock = INVALID_SOCKET;
    }

    return 0;
}

// Send a mouse move event to the streaming machine
int LiSendMouseMoveEvent(short deltaX, short deltaY) {
    PPACKET_HOLDER holder;
    int err;

    if (!initialized) {
        return -2;
    }

    if (deltaX == 0 && deltaY == 0) {
        return 0;
    }

    holder = malloc(sizeof(*holder));
    if (holder == NULL) {
        return -1;
    }

    holder->packetLength = sizeof(NV_REL_MOUSE_MOVE_PACKET);
    holder->packet.mouseMoveRel.header.packetType = htonl(PACKET_TYPE_REL_MOUSE_MOVE);
    holder->packet.mouseMoveRel.magic = MOUSE_MOVE_REL_MAGIC;
    // On Gen 5 servers, the header code is incremented by one
    if (AppVersionQuad[0] >= 5) {
        holder->packet.mouseMoveRel.magic++;
    }
    holder->packet.mouseMoveRel.deltaX = htons(deltaX);
    holder->packet.mouseMoveRel.deltaY = htons(deltaY);

    err = LbqOfferQueueItem(&packetQueue, holder, &holder->entry);
    if (err != LBQ_SUCCESS) {
        free(holder);
    }

    return err;
}

// Send a mouse position update to the streaming machine
int LiSendMousePositionEvent(short x, short y, short referenceWidth, short referenceHeight) {
    PPACKET_HOLDER holder;
    int err;

    if (!initialized) {
        return -2;
    }

    holder = malloc(sizeof(*holder));
    if (holder == NULL) {
        return -1;
    }

    holder->packetLength = sizeof(NV_ABS_MOUSE_MOVE_PACKET);
    holder->packet.mouseMoveAbs.header.packetType = htonl(PACKET_TYPE_ABS_MOUSE_MOVE);
    holder->packet.mouseMoveAbs.magic = MOUSE_MOVE_ABS_MAGIC;
    holder->packet.mouseMoveAbs.x = htons(x);
    holder->packet.mouseMoveAbs.y = htons(y);
    holder->packet.mouseMoveAbs.unused = 0;

    // There appears to be a rounding error in GFE's scaling calculation which prevents
    // the cursor from reaching the far edge of the screen when streaming at smaller
    // resolutions with a higher desktop resolution (like streaming 720p with a desktop
    // resolution of 1080p, or streaming 720p/1080p with a desktop resolution of 4K).
    // Subtracting one from the reference dimensions seems to work around this issue.
    holder->packet.mouseMoveAbs.width = htons(referenceWidth - 1);
    holder->packet.mouseMoveAbs.height = htons(referenceHeight - 1);

    err = LbqOfferQueueItem(&packetQueue, holder, &holder->entry);
    if (err != LBQ_SUCCESS) {
        free(holder);
    }

    return err;
}

// Send a mouse button event to the streaming machine
int LiSendMouseButtonEvent(char action, int button) {
    PPACKET_HOLDER holder;
    int err;

    if (!initialized) {
        return -2;
    }

    holder = malloc(sizeof(*holder));
    if (holder == NULL) {
        return -1;
    }

    holder->packetLength = sizeof(NV_MOUSE_BUTTON_PACKET);
    holder->packet.mouseButton.header.packetType = htonl(PACKET_TYPE_MOUSE_BUTTON);
    holder->packet.mouseButton.action = action;
    if (AppVersionQuad[0] >= 5) {
        holder->packet.mouseButton.action++;
    }
    holder->packet.mouseButton.button = htonl(button);

    err = LbqOfferQueueItem(&packetQueue, holder, &holder->entry);
    if (err != LBQ_SUCCESS) {
        free(holder);
    }

    return err;
}

// Send a key press event to the streaming machine
int LiSendKeyboardEvent(short keyCode, char keyAction, char modifiers) {
    PPACKET_HOLDER holder;
    int err;

    if (!initialized) {
        return -2;
    }

    holder = malloc(sizeof(*holder));
    if (holder == NULL) {
        return -1;
    }

    // For proper behavior, the MODIFIER flag must not be set on the modifier key down event itself
    // for the extended modifiers on the right side of the keyboard. If the MODIFIER flag is set,
    // GFE will synthesize an errant key down event for the non-extended key, causing that key to be
    // stuck down after the extended modifier key is raised. For non-extended keys, we must set the
    // MODIFIER flag for correct behavior.
    switch (keyCode & 0xFF) {
    case 0x5B: // VK_LWIN
    case 0x5C: // VK_RWIN
        // Any keyboard event with the META modifier flag is dropped by all known GFE versions.
        // This prevents us from sending shortcuts involving the meta key (Win+X, Win+Tab, etc).
        // The catch is that the meta key event itself would actually work if it didn't set its
        // own modifier flag, so we'll clear that here. This should be safe even if a new GFE
        // release comes out that stops dropping events with MODIFIER_META flag.
        modifiers &= ~MODIFIER_META;
        break;

    case 0xA0: // VK_LSHIFT
        modifiers |= MODIFIER_SHIFT;
        break;
    case 0xA1: // VK_RSHIFT
        modifiers &= ~MODIFIER_SHIFT;
        break;

    case 0xA2: // VK_LCONTROL
        modifiers |= MODIFIER_CTRL;
        break;
    case 0xA3: // VK_RCONTROL
        modifiers &= ~MODIFIER_CTRL;
        break;

    case 0xA4: // VK_LMENU
        modifiers |= MODIFIER_ALT;
        break;
    case 0xA5: // VK_RMENU
        modifiers &= ~MODIFIER_ALT;
        break;

    default:
        // No fixups
        break;
    }

    holder->packetLength = sizeof(NV_KEYBOARD_PACKET);
    holder->packet.keyboard.header.packetType = htonl(PACKET_TYPE_KEYBOARD);
    holder->packet.keyboard.keyAction = keyAction;
    holder->packet.keyboard.zero1 = 0;
    holder->packet.keyboard.keyCode = keyCode;
    holder->packet.keyboard.modifiers = modifiers;
    holder->packet.keyboard.zero2 = 0;

    err = LbqOfferQueueItem(&packetQueue, holder, &holder->entry);
    if (err != LBQ_SUCCESS) {
        free(holder);
    }

    return err;
}

static int sendControllerEventInternal(short controllerNumber, short activeGamepadMask,
    short buttonFlags, unsigned char leftTrigger, unsigned char rightTrigger,
    short leftStickX, short leftStickY, short rightStickX, short rightStickY)
{
    PPACKET_HOLDER holder;
    int err;

    if (!initialized) {
        return -2;
    }

    holder = malloc(sizeof(*holder));
    if (holder == NULL) {
        return -1;
    }

    if (AppVersionQuad[0] == 3) {
        // Generation 3 servers don't support multiple controllers so we send
        // the legacy packet
        holder->packetLength = sizeof(NV_CONTROLLER_PACKET);
        holder->packet.controller.header.packetType = htonl(PACKET_TYPE_CONTROLLER);
        holder->packet.controller.headerA = __bswap32(C_HEADER_A);
        holder->packet.controller.headerB = __bswap16(C_HEADER_B);
        holder->packet.controller.buttonFlags = __bswap16(buttonFlags);
        holder->packet.controller.leftTrigger = leftTrigger;
        holder->packet.controller.rightTrigger = rightTrigger;
        holder->packet.controller.leftStickX = __bswap16(leftStickX);
        holder->packet.controller.leftStickY = __bswap16(leftStickY);
        holder->packet.controller.rightStickX = __bswap16(rightStickX);
        holder->packet.controller.rightStickY = __bswap16(rightStickY);
        holder->packet.controller.tailA = __bswap32(C_TAIL_A);
        holder->packet.controller.tailB = __bswap16(C_TAIL_B);
    }
    else {
        // Generation 4+ servers support passing the controller number
        holder->packetLength = sizeof(NV_MULTI_CONTROLLER_PACKET);
        holder->packet.multiController.header.packetType = htonl(PACKET_TYPE_MULTI_CONTROLLER);
        holder->packet.multiController.headerA = MC_HEADER_A;
        // On Gen 5 servers, the header code is decremented by one
        if (AppVersionQuad[0] >= 5) {
            holder->packet.multiController.headerA--;
        }
        holder->packet.multiController.headerA = __bswap32(holder->packet.multiController.headerA);
        holder->packet.multiController.headerB = __bswap16(MC_HEADER_B);
        holder->packet.multiController.controllerNumber = __bswap16(controllerNumber);
        holder->packet.multiController.activeGamepadMask = __bswap16(activeGamepadMask);
        holder->packet.multiController.midB = __bswap16(MC_MID_B);
        holder->packet.multiController.buttonFlags = __bswap16(buttonFlags);
        holder->packet.multiController.leftTrigger = leftTrigger;
        holder->packet.multiController.rightTrigger = rightTrigger;
        holder->packet.multiController.leftStickX = __bswap16(leftStickX);
        holder->packet.multiController.leftStickY = __bswap16(leftStickY);
        holder->packet.multiController.rightStickX = __bswap16(rightStickX);
        holder->packet.multiController.rightStickY = __bswap16(rightStickY);
        holder->packet.multiController.tailA = __bswap32(MC_TAIL_A);
        holder->packet.multiController.tailB = __bswap16(MC_TAIL_B);
    }

    err = LbqOfferQueueItem(&packetQueue, holder, &holder->entry);
    if (err != LBQ_SUCCESS) {
        free(holder);
    }

    return err;
}

// Send a controller event to the streaming machine
int LiSendControllerEvent(short buttonFlags, unsigned char leftTrigger, unsigned char rightTrigger,
    short leftStickX, short leftStickY, short rightStickX, short rightStickY)
{
    return sendControllerEventInternal(0, 0x1, buttonFlags, leftTrigger, rightTrigger,
        leftStickX, leftStickY, rightStickX, rightStickY);
}

// Send a controller event to the streaming machine
int LiSendMultiControllerEvent(short controllerNumber, short activeGamepadMask,
    short buttonFlags, unsigned char leftTrigger, unsigned char rightTrigger,
    short leftStickX, short leftStickY, short rightStickX, short rightStickY)
{
    return sendControllerEventInternal(controllerNumber, activeGamepadMask,
        buttonFlags, leftTrigger, rightTrigger,
        leftStickX, leftStickY, rightStickX, rightStickY);
}

// Send a high resolution scroll event to the streaming machine
int LiSendHighResScrollEvent(short scrollAmount) {
    PPACKET_HOLDER holder;
    int err;

    if (!initialized) {
        return -2;
    }

    if (scrollAmount == 0) {
        return 0;
    }

    holder = malloc(sizeof(*holder));
    if (holder == NULL) {
        return -1;
    }

    holder->packetLength = sizeof(NV_SCROLL_PACKET);
    holder->packet.scroll.header.packetType = htonl(PACKET_TYPE_SCROLL);
    holder->packet.scroll.magicA = MAGIC_A;
    // On Gen 5 servers, the header code is incremented by one
    if (AppVersionQuad[0] >= 5) {
        holder->packet.scroll.magicA++;
    }
    holder->packet.scroll.zero1 = 0;
    holder->packet.scroll.zero2 = 0;
    holder->packet.scroll.scrollAmt1 = htons(scrollAmount);
    holder->packet.scroll.scrollAmt2 = holder->packet.scroll.scrollAmt1;
    holder->packet.scroll.zero3 = 0;

    err = LbqOfferQueueItem(&packetQueue, holder, &holder->entry);
    if (err != LBQ_SUCCESS) {
        free(holder);
    }

    return err;
}

// Send a scroll event to the streaming machine
int LiSendScrollEvent(signed char scrollClicks) {
    return LiSendHighResScrollEvent(scrollClicks * 120);
}
