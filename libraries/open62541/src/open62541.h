/* open62541 umbrella header.
 *
 * Include THIS, not the nested <open62541/...> paths, from Arduino sources:
 * arduino-cli discovers a library by basename at src/ root, so a nested
 * include resolves to nothing and the precompiled archive is silently left
 * out of the link. Including this file first puts <lib>/src on the include
 * path, after which the nested headers below resolve normally.
 */
#ifndef OPEN62541_UMBRELLA_H
#define OPEN62541_UMBRELLA_H
#include <open62541/config.h>
#include <open62541/types.h>
#include <open62541/server.h>
#include <open62541/server_config_default.h>
#include <open62541/plugin/eventloop.h>
#include <open62541/plugin/log.h>
#include <open62541/plugin/nodestore.h>
#endif /* OPEN62541_UMBRELLA_H */
