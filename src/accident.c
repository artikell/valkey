#include "server.h"

#define GENERATE_COMMAND(originalCommand) \
    void originalCommand(client *c) {                    \
        addReplyError(c, "Error unsupported command");   \
    }


#ifdef _ENGINE_DISABLE_SLOWLOG
GENERATE_COMMAND(slowlogCommand)
#endif

