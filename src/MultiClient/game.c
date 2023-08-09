#include "multi.h"

static void gameServerClose(Game* game)
{
    if (game->socketServer != INVALID_SOCKET)
    {
        closesocket(game->socketServer);
        game->socketServer = INVALID_SOCKET;
    }

    game->timeout = 0;
    game->txBufferPos = 0;
    game->rxBufferSize = 0;
}

static void gameClose(Game* game)
{
    game->valid = 0;
    if (game->socketServer)
    {
        closesocket(game->socketServer);
        game->socketServer = INVALID_SOCKET;
    }

    if (game->socketApi)
    {
        closesocket(game->socketApi);
        game->socketApi = INVALID_SOCKET;
    }

    free(game->txBuffer);
    free(game->rxBuffer);
    free(game->entries);
}

static void gameServerReconnect(Game* game)
{
    printf("Disconnected from server, reconnecting...\n");
    gameServerClose(game);
    game->state = STATE_CONNECT;
}

void gameInit(Game* game, SOCKET sock)
{
    /* Set initial values */
    game->valid = 1;
    game->state = STATE_INIT;
    game->delay = 0;
    game->nopAcc = 0;
    game->timeout = 0;
    game->apiError = 0;
    game->socketApi = sock;
    game->socketServer = INVALID_SOCKET;

    game->txBufferCapacity = 4096;
    game->txBuffer = malloc(game->txBufferCapacity);
    game->txBufferSize = 0;
    game->txBufferPos = 0;

    game->rxBuffer = malloc(256);
    game->rxBufferSize = 0;

    game->entriesCount = 0;
    game->entriesCapacity = 256;
    game->entries = malloc(game->entriesCapacity * sizeof(LedgerFullEntry));

    /* Log */
    printf("Game created\n");
}

static void gameLoadLedger(Game* game)
{

}

static void gameLoadApiData(Game* game)
{
    uint32_t uuidAddr;

    uuidAddr = protocolRead32(game, game->apiNetAddr + 0x00);
    for (int i = 0; i < 16; ++i)
        game->uuid[i] = protocolRead8(game, uuidAddr + i);

    if (!game->apiError)
    {
        gameLoadLedger(game);
        game->state = STATE_CONNECT;
    }
}

static void writeLedger(Game* game, uint64_t key, const void* extra, uint8_t extraSize)
{
    gameTransfer(game, "\x01", 1);
    gameTransfer(game, &key, 8);
    gameTransfer(game, &extraSize, 1);
    gameTransfer(game, extra, extraSize);
}

static uint64_t crc64(const void* data, size_t size)
{
    uint64_t crc;
    crc = 0;
    for (size_t i = 0; i < size; ++i)
    {
        crc ^= ((uint64_t)((uint8_t*)data)[i] << 56);
        for (int j = 0; j < 8; ++j)
        {
            if (crc & 0x8000000000000000)
                crc = (crc << 1) ^ 0x42f0e1eba9ea3693;
            else
                crc <<= 1;
        }
    }

    return crc;
}

static uint64_t itemKey(uint32_t checkKey, uint8_t gameId, uint8_t playerFrom, uint32_t entriesCount)
{
    char buffer[0x10];

    memset(buffer, 0, sizeof(buffer));
    memcpy(buffer + 0x00, &checkKey, 4);
    memcpy(buffer + 0x04, &gameId, 1);
    memcpy(buffer + 0x05, &playerFrom, 1);
    memcpy(buffer + 0x06, &entriesCount, 4);

    return crc64(buffer, sizeof(buffer));
}

static void writeItemLedger(Game* game, uint8_t playerFrom, uint8_t playerTo, uint8_t gameId, uint32_t k, uint16_t gi, uint16_t flags)
{
    uint64_t key;
    char payload[0x10];

    /* Build the key */
    key = itemKey(k, gameId, playerFrom, (flags & (1 << 2)) ? game->entriesCount : 0xffffffff);

    /* Build the payload */
    memset(payload, 0, sizeof(payload));
    memcpy(payload + 0x00, &playerFrom, 1);
    memcpy(payload + 0x01, &playerTo, 1);
    memcpy(payload + 0x02, &gameId, 1);
    memcpy(payload + 0x04, &k, 4);
    memcpy(payload + 0x08, &gi, 2);
    memcpy(payload + 0x0a, &flags, 2);

    /* Write the ledger */
    writeLedger(game, key, payload, sizeof(payload));
}

static void gameApiItemOut(Game* game)
{
    uint32_t itemBase;
    uint8_t playerFrom;
    uint8_t playerTo;
    uint8_t gameId;
    uint32_t key;
    uint16_t gi;
    uint16_t flags;

    itemBase = game->apiNetAddr + 0x0c;
    playerFrom = protocolRead8(game, itemBase + 0x00);
    playerTo = protocolRead8(game, itemBase + 0x01);
    gameId = protocolRead8(game, itemBase + 0x02);
    key = protocolRead32(game, itemBase + 0x04);
    gi = protocolRead16(game, itemBase + 0x08);
    flags = protocolRead16(game, itemBase + 0x0a);

    if (game->apiError)
        return;

    printf("ITEM OUT - FROM: %d, TO: %d, GAME: %d, KEY: %04X, GI: %04X, FLAGS: %04X\n", playerFrom, playerTo, gameId, key, gi, flags);

    /* Write */
    writeItemLedger(game, playerFrom, playerTo, gameId, key, gi, flags);

    /* Mark the item as sent */
    protocolWrite8(game, game->apiNetAddr + 0x08, 0x00);
}

static void gameApiApplyLedger(Game* game)
{
    uint32_t entryId;
    uint32_t cmdBase;
    uint16_t tmp16;
    uint32_t tmp32;
    LedgerFullEntry* fe;

    entryId = protocolRead32(game, game->apiNetAddr + 0x04);
    if (entryId == 0xffffffff)
        return;
    if (entryId >= game->entriesCount)
        return;
    if (game->apiError)
        return;

    /* Apply the ledger entry */
    printf("LEDGER APPLY #%d\n", entryId);
    fe = game->entries + entryId;
    protocolWrite8(game, game->apiNetAddr + 0x18, 0x01);
    cmdBase = game->apiNetAddr + 0x1c;
    protocolWrite8(game, cmdBase + 0x00, fe->data[0x00]); // playerFrom
    protocolWrite8(game, cmdBase + 0x01, fe->data[0x01]); // playerTo
    protocolWrite8(game, cmdBase + 0x02, fe->data[0x02]); // gameId
    memcpy(&tmp32, fe->data + 0x04, 4);
    protocolWrite32(game, cmdBase + 0x04, tmp32); // key
    memcpy(&tmp16, fe->data + 0x08, 2);
    protocolWrite16(game, cmdBase + 0x08, tmp16); // gi
    memcpy(&tmp16, fe->data + 0x0a, 2);
    protocolWrite16(game, cmdBase + 0x0a, tmp16); // flags
}

static void gameApiTick(Game* game)
{
    uint8_t gameOpOut;
    uint8_t gameOpIn;

    if (game->state == STATE_INIT)
        gameLoadApiData(game);
    if (game->state == STATE_READY)
    {
        gameOpOut = protocolRead8(game, game->apiNetAddr + 0x08);
        gameOpIn = protocolRead8(game, game->apiNetAddr + 0x18);
        if (game->apiError)
            return;

        if (gameOpOut == 0x02)
        {
            gameApiItemOut(game);
        }

        if (gameOpIn == 0x00)
        {
            gameApiApplyLedger(game);
        }
    }
}

static int gameProcessInputRx(Game* game, uint32_t size)
{
    int ret;

    while (game->rxBufferSize < size)
    {
        ret = recv(game->socketServer, game->rxBuffer + game->rxBufferSize, size - game->rxBufferSize, 0);
        if (ret == 0)
        {
            gameServerReconnect(game);
            return 0;
        }
        if (ret < 0)
            return 0;
        game->rxBufferSize += ret;
        game->timeout = 0;
    }

    //printf("DATA: ");
    //for (uint32_t i = 0; i < game->rxBufferSize; ++i)
    //    printf("%02x ", (uint8_t)game->rxBuffer[i]);
    //printf("\n");

    return 1;
}

static int gameProcessRxLedgerEntry(Game* game)
{
    LedgerFullEntry fe;
    uint8_t extraSize;

    /* Fetch the full header */
    if (!gameProcessInputRx(game, 10))
        return 0;

    extraSize = (uint8_t)game->rxBuffer[9];
    if (!gameProcessInputRx(game, 10 + extraSize))
        return 0;

    /* We have a full ledger entry */
    memset(&fe, 0, sizeof(fe));
    memcpy(&fe.key, game->rxBuffer + 1, 8);
    fe.size = extraSize;
    memcpy(fe.data, game->rxBuffer + 10, extraSize);

    printf("LEDGER ENTRY: %d bytes\n", extraSize);
    game->rxBufferSize = 0;

    /* Save the ledger entry */
    if (game->entriesCount == game->entriesCapacity)
    {
        game->entriesCapacity *= 2;
        game->entries = realloc(game->entries, game->entriesCapacity * sizeof(LedgerFullEntry));
    }
    memcpy(game->entries + game->entriesCount, &fe, sizeof(LedgerFullEntry));
    game->entriesCount++;

    return 1;
}

static void gameProcessInput(Game* game)
{
    uint8_t op;

    for (;;)
    {
        if (game->rxBufferSize == 0)
        {
            if (!gameProcessInputRx(game, 1))
                return;
        }

        op = (uint8_t)game->rxBuffer[0];

        switch (op)
        {
        case 0: // NOP
            game->rxBufferSize = 0;
            break;
        case 1:
            if (!gameProcessRxLedgerEntry(game))
                return;
            break;
        default:
            fprintf(stderr, "Unknown opcode: %02x\n", op);
            game->rxBufferSize = 0;
            break;
        }
    }
}

static void gameProcessTransfer(Game* game)
{
    int ret;

    for (;;)
    {
        if (game->txBufferPos == game->txBufferSize)
        {
            game->txBufferPos = 0;
            game->txBufferSize = 0;
            return;
        }

        ret = send(game->socketServer, game->txBuffer + game->txBufferPos, game->txBufferSize - game->txBufferPos, 0);
        if (ret > 0)
        {
            game->txBufferPos += ret;
        }
        else
            break;
    }
}

static void gameServerJoin(Game* game)
{
    char buf[20];
    uint32_t ledgerBase;

    ledgerBase = game->entriesCount;
    memcpy(buf, game->uuid, 16);
    memcpy(buf + 16, &ledgerBase, 4);
    if (send(game->socketServer, buf, 20, 0) != 20)
    {
        printf("Unable to send join message\n");
        gameServerReconnect(game);
        return;
    }
    game->state = STATE_READY;
}

static void gameServerConnect(App* app, Game* game)
{
    struct addrinfo hints;
    struct addrinfo* result;
    struct addrinfo* ptr;
    u_long mode;
    int ret;
    char buf[16];

    /* Resolve the server address and port */
    memset(&hints, 0, sizeof(hints));
    hints.ai_family     = AF_UNSPEC;
    hints.ai_socktype   = SOCK_STREAM;
    hints.ai_protocol   = IPPROTO_TCP;
    snprintf(buf, sizeof(buf), "%d", app->serverPort);

    ret = getaddrinfo(app->serverHost, buf, &hints, &result);
    if (ret != 0)
    {
        printf("getaddrinfo failed: %d\n", ret);
        return;
    }

    /* Attempt to connect to an address until one succeeds */
    game->socketServer = INVALID_SOCKET;
    for (ptr = result; ptr != NULL; ptr = ptr->ai_next)
    {
        /* Create a SOCKET for connecting to server */
        game->socketServer = socket(ptr->ai_family, ptr->ai_socktype, ptr->ai_protocol);
        if (game->socketServer == INVALID_SOCKET)
            continue;

        /* Connect to server */
        ret = connect(game->socketServer, ptr->ai_addr, (int)ptr->ai_addrlen);
        if (ret == SOCKET_ERROR)
        {
            closesocket(game->socketServer);
            game->socketServer = INVALID_SOCKET;
            continue;
        }

        /* Make the socket blocking */
        mode = 0;
        ioctlsocket(game->socketServer, FIONBIO, &mode);

        /* Send the initial message */
        if (send(game->socketServer, "OoTMM", 5, 0) != 5)
        {
            closesocket(game->socketServer);
            game->socketServer = INVALID_SOCKET;
            continue;
        }

        /* Get the initial reply */
        if (recv(game->socketServer, buf, 5, 0) != 5)
        {
            closesocket(game->socketServer);
            game->socketServer = INVALID_SOCKET;
            continue;
        }
        if (memcmp(buf, "OoTMM", 5))
        {
            closesocket(game->socketServer);
            game->socketServer = INVALID_SOCKET;
            continue;
        }

        /* Make the socket non blocking */
        mode = 1;
        ioctlsocket(game->socketServer, FIONBIO, &mode);

        break;
    }
    freeaddrinfo(result);

    if (game->socketServer == INVALID_SOCKET)
    {
        printf("Unable to connect to server at %s:%d\n", app->serverHost, app->serverPort);
        game->delay = 100;
        return;
    }

    /* Log */
    printf("Connected to server at %s:%d\n", app->serverHost, app->serverPort);
    game->state = STATE_JOIN;
    gameServerJoin(game);
}

static void gameServerTick(App* app, Game* game)
{
    if (game->apiError)
        return;

    if (game->delay)
    {
        --game->delay;
        return;
    }

    game->timeout++;
    if (game->timeout >= 1500)
    {
        gameServerReconnect(game);
        return;
    }

    switch (game->state)
    {
    case STATE_INIT:
        break;
    case STATE_CONNECT:
        gameServerConnect(app, game);
        break;
    case STATE_JOIN:
        break;
    case STATE_READY:
        if (game->txBufferSize == 0 && game->nopAcc >= 100)
        {
            game->nopAcc = 0;
            gameTransfer(game, "\x00", 1);
        }
        else
            ++game->nopAcc;
        gameProcessTransfer(game);
        gameProcessInput(game);
        break;
    }
}

void gameTick(App* app, Game* game)
{
    if (apiContextLock(game))
    {
        gameApiTick(game);
        apiContextUnlock(game);
    }
    gameServerTick(app, game);
    if (game->apiError)
    {
        printf("Game disconnected\n");
        gameClose(game);
    }
}

void gameTransfer(Game* game, const void* data, uint32_t size)
{
    while (game->txBufferSize + size > game->txBufferCapacity)
    {
        game->txBufferCapacity *= 2;
        game->txBuffer = realloc(game->txBuffer, game->txBufferCapacity);
    }
    memcpy(game->txBuffer + game->txBufferSize, data, size);
    game->txBufferSize += size;
}
