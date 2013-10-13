cocaine-native-proxy
====================

Cocaine HTTP proxy

Example config
==============

```JSON
{
    "endpoints": [
        "0.0.0.0:8080"
    ],
    "daemon": {
        "monitor-port": 20000
    },
    "backlog": 2048,
    "threads": 2,
    "application": {
        "locators": ["127.0.0.1:10053"],
        "logging_prefix": "cocaine-proxy-01",
        "service_pool": 10,
        "reconnect_timeout": 180,
        "request_timeout": 5
    }
}
```
