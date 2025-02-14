Usbmodem daemon provides ubus API for each own interface.

All methods must to be called on this path:
`usbmodem.<interface_name>`

Where `<interface_name>` is name of modem interface in openwrt.

**List all available modems:**
```
$ ubus list usbmodem.*
usbmodem.LTE
```

# send_command

Sending custom AT command to modem.

**Arguments:**
| Name | Type | Description |
|---|---|---|
| command | string | AT command |
| timeout | int | Timeout for command response. Used default timeout, when omitted. |

**Response:**
| Name | Type | Description |
|---|---|---|
| success | string | True, if command success |
| response | string | Response of AT command. Can be empty, when timeout occured. |

**Example:**
```
$ ubus call usbmodem.LTE send_command '{"command": "AT+CGMI"}'
{
	"response": "+CGMI: \"Huawei\"\nOK",
	"success": true
}
```

# send_ussd

Sending USSD query to network.

**Arguments:**
| Name | Type | Description |
|---|---|---|
| query | string | USSD query, for example: `*777#` |
| answer | string | Answer for ussd, when previous USSD respond with code=1. |

You can pass **query** for new ussd query and **answer** for answer to previous query. But not at the same time.

**Response:**
| Name | Type | Description |
|---|---|---|
| code | int | USSD response code:<br>0 - Success<br>1 - Success, but wait answer<br>2 - Discard by network |
| response | string | Decoded USSD response. |
| error | string | Error description, when request failed. |

**Example:**
```
$ ubus call usbmodem.LTE send_ussd '{"query": "*777#"}'
{
	"code": 0,
	"response": "Your balance is 1.00 usd."
}
$ ubus call usbmodem.LTE send_ussd '{"query": "wefwefw"}'
{
	"error": "Not valid ussd command."
}
$ ubus call usbmodem.LTE send_ussd '{"query": "*123#"}'
{
	"code": 1,
	"response": "1) check balance\n2) show my number\n0) cancel"
}
$ ubus call usbmodem.LTE send_ussd '{"answer": "1"}'
{
	"code": 0,
	"response": "Your balance is 13.77 usd."
}
```

# cancel_ussd

Canceling current USSD session (when previous query respond with code=1 and waiting for reply)

**Example:**
```
$ ubus call usbmodem.LTE cancel_ussd
```

# read_sms

Reading sms from modem.

**Arguments:**
| Name | Type | Description |
|---|---|---|
| dir | int | 0 - unread messages<br>1 - read messages<br>2 - unsent messages<br>3 - sent messages<br>4 - all messages |

**Response:**
| Name | Type | Description |
|---|---|---|
| messages | array | Array of message objects. |

**Each message object**
| Name | Type | Description |
|---|---|---|
| hash | uint | uniq id, hash of PDU and message index |
| addr | string | From/to phone number |
| type | int | 0 - incoming message<br>1 - outgoing message |
| dir | int | 0 - unread messages<br>1 - read messages<br>2 - unsent messages<br>3 - sent messages<br>4 - all messages |
| time | uint | Unix timestamp. For outgoing messages always 0 |
| invalid | bool | True, when message is not decoded properly |
| unread | bool | True, when message is not read |
| parts | array | Text parts of message |

**Each message part**
| Name | Type | Description |
|---|---|---|
| id | int | ID of message part in modem |
| text | string | Text content |

**Example:**
```
$ ubus call usbmodem.LTE read_sms
{
	"messages": [
		{
			"addr": "+11232342334",
			"dir": 1,
			"id": 2884079854,
			"invalid": false,
			"parts": [
				{
					"id": 1,
					"text": "Test sms "
				}
			],
			"time": 1630060386,
			"type": 0,
			"unread": false
		},
		{
			"addr": "+112323434332",
			"dir": 1,
			"id": 1397996874,
			"invalid": false,
			"parts": [
				{
					"id": 3,
					"text": "Lorem ipsum dolor sit amet, consectetur adipiscing elit, sed do eiusmod tempor incididunt ut labore et dolore magna aliqua. Ut enim ad minim veniam, quis"
				},
				{
					"id": 4,
					"text": " nostrud exercitation ullamco laboris nisi ut aliquip ex ea commodo consequat. Duis aute irure dolor in reprehenderit in voluptate velit esse cillum dolo"
				},
				{
					"id": 5,
					"text": "re eu fugiat nulla pariatur. Excepteur sint occaecat cupidatat non proident, sunt in culpa qui officia deserunt mollit anim id est laborum."
				}
			],
			"time": 1630245213,
			"type": 0,
			"unread": false
		}
	]
}
```

# delete_sms

Delete sms from modem.

**Arguments:**
| Name | Type | Description |
|---|---|---|
| ids | array | Array of **id** from sms object in **parts** field. |

**Response:**
| Name | Type | Description |
|---|---|---|
| errors | object | Error for each **id** |
| result | object | Status of deletion for each **id** |

**Example:**
```
$ ubus call usbmodem.LTE delete_sms '{"ids": [2]}'
{
	"errors": false,
	"result": {
		"2": true
	}
}
$ ubus call usbmodem.LTE delete_sms '{"ids": [2]}'
{
	"errors": {
		"2": "Message #2 failed to delete."
	},
	"result": {
		"2": false
	}
}
```
