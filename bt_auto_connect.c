
/*
  *  BlueZ - Bluetooth protocol stack for Linux
 *
 *  Copyright (C) 2011  Nokia Corporation
 *
 *
 *  This program is free software; you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation; either version 2 of the License, or
 *  (at your option) any later version.
 *
 *  This program is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *  GNU General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License
 *  along with this program; if not, write to the Free Software
 *  Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA  02110-1301  USA
 *
 */

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include <string.h>
#include <stdlib.h>
#include <errno.h>
#include <stdio.h>
#include <assert.h>
#include <glib.h>
#include <getopt.h>
#include <signal.h>
#include <poll.h>
#include <pthread.h>
#include "lib/uuid.h"

#include <sys/ioctl.h>
#include <btio/btio.h>
#include <sys/time.h>

#include "attrib/att.h"
#include "attrib/gattrib.h"
#include "attrib/gatt.h"
#include "attrib/gatttool.h"

#include "lib/bluetooth.h"
#include "lib/hci.h"
#include "lib/hci_lib.h"

static GIOChannel *iochannel = NULL;
static GAttrib *attrib = NULL;
static GMainLoop *event_loop;

static gchar *opt_src = NULL;
static gchar *opt_dst = NULL;
static gchar *opt_dst_type = NULL;
static gchar *opt_sec_level = NULL;

static const int opt_psm = 0;
static int opt_mtu = 0;
static struct hci_dev_info di;

static int opt_handle = -1;
static gchar *opt_value = NULL;

static gboolean opt_listen = FALSE;
static gboolean got_error = FALSE;

static GSourceFunc operation;
gpointer user_data;
struct le_devices le_devices;
uint8_t flag_connect = 0;
#define for_each_opt(opt, long, short) while ((opt=getopt_long(argc, argv, short ? short:"+", long, NULL)) != -1)

#define LE_LINK		0x03
#define FLAGS_AD_TYPE 0x01
#define FLAGS_LIMITED_MODE_BIT 0x01
#define FLAGS_GENERAL_MODE_BIT 0x02
#define EIR_FLAGS                   0x01  /* flags */
#define EIR_UUID16_SOME             0x02  /* 16-bit UUID, more available */
#define EIR_UUID16_ALL              0x03  /* 16-bit UUID, all listed */
#define EIR_UUID32_SOME             0x04  /* 32-bit UUID, more available */
#define EIR_UUID32_ALL              0x05  /* 32-bit UUID, all listed */
#define EIR_UUID128_SOME            0x06  /* 128-bit UUID, more available */
#define EIR_UUID128_ALL             0x07  /* 128-bit UUID, all listed */
#define EIR_NAME_SHORT              0x08  /* shortened local name */
#define EIR_NAME_COMPLETE           0x09  /* complete local name */
#define EIR_TX_POWER                0x0A  /* transmit power level */
#define EIR_APPERANCE		    	0x19  /* Appearance*/
#define EIR_DEVICE_ID               0x10  /* device ID */
#define EIR_SLAVE_CONN_INTVAL	    0X12  /* Slave connectoin inteval*/
#define CHARACTERISTICS_HANDLE		0X0017
#define SIZE_1024B 					1024
#define MANU_TYPE					0x005C
#define EIR_MANUFACTURE_SPECIFIC    0xFF

#define BLUETOOTH_DATABASE "devicelist.db"

static void char_write_req_cb(guint8 status, const guint8 *pdu, guint16 plen,
														gpointer user_data);
static gboolean char_write_auto( gpointer user_data);
static volatile int signal_received = 0;
int unknown_addr = 0;
struct le_devices
{
    bdaddr_t bdaddr;
    uint8_t manufacturer;
    uint8_t status;
    uint8_t type;
};

struct characteristic_data {
	GAttrib *attrib;
	//uint16_t orig_start;
	uint16_t start;
	uint16_t end;
	bt_uuid_t uuid;
};


static enum state {
	STATE_DISCONNECTED=0,
	STATE_CONNECTING=1,
	STATE_CONNECTED=2
} conn_state;

static const char 
  *tag_RESPONSE  = "respone",
  *tag_ERRCODE   = "code",
  *tag_HANDLE    = "handle",
  *tag_DATA      = "data",
  *tag_CONNSTATE = "state",
  *tag_SEC_LEVEL = "sec",
  *tag_MTU       = "mtu",
  *tag_DEVICE    = "dst";

static const char
  *rsp_ERROR     = "error",
  *rsp_STATUS    = "status",
  *rsp_NOTIFY    = "ntfy",
  *rsp_IND       = "ind",
  *rsp_WRITE     = "wr";

static const char
  *err_CONN_FAIL = "connect fail",
  *err_COMM_ERR  = "com error",
  *err_PROTO_ERR = "protocol error",
  //*err_BAD_CMD   = "can not understand cmd",
  *err_BAD_PARAM = "do not understand parameter";

static const char 
  *st_DISCONNECTED = "disc",
  *st_CONNECTING   = "tryconn",
  *st_CONNECTED    = "conn";

struct cmd_option {

	const char *str ;
	int option_num ;

} cmd_option;

static void resp_begin(const char *rsptype)
{
  printf(" %s:%s", tag_RESPONSE, rsptype);
}

static void send_sym(const char *tag, const char *val)
{
  printf(" %s:%s", tag, val);
}

static void send_uint(const char *tag, unsigned int val)
{
  printf(" %s=h%X", tag, val);
}

static void send_str(const char *tag, const char *val)
{
  printf(" %s='%s", tag, val);
}

static void send_data(const unsigned char *val, size_t len)
{
  printf(" %s=b", tag_DATA);
  while ( len-- > 0 )
    printf("%02X", *val++);
}

static void resp_end()
{
  printf("\n");
  fflush(stdout);
}

static void resp_error(const char *errcode)
{
  resp_begin(rsp_ERROR);
  printf("\n");
  send_sym(tag_ERRCODE, errcode);
  printf("\n");
  resp_end();
}

static void print_dev_hdr(struct hci_dev_info *di)
{
	static int hdr = -1;
	char addr[18];

	if (hdr == di->dev_id)
		return;
	hdr = di->dev_id;

	ba2str(&di->bdaddr, addr);

	printf("%s:\tType: %s  Bus: %s\n", di->name,
					hci_typetostr((di->type & 0x30) >> 4),
					hci_bustostr(di->type & 0x0f));
	printf("\tBD Address: %s  ACL MTU: %d:%d  SCO MTU: %d:%d\n",
					addr, di->acl_mtu, di->acl_pkts,
						di->sco_mtu, di->sco_pkts);
}

static inline uint16_t att_get_u16(const void *ptr)
{
        const uint16_t *u16_ptr = ptr;
        return btohs(bt_get_unaligned(u16_ptr));
}

static void cmd_status(int parameter, int argcp, char **argvp)
{
  resp_begin(rsp_STATUS);
  switch(conn_state)
  {
    case STATE_CONNECTING:
      send_sym(tag_CONNSTATE, st_CONNECTING);
      send_str(tag_DEVICE, opt_dst);
      break;

    case STATE_CONNECTED:
      send_sym(tag_CONNSTATE, st_CONNECTED);
      send_str(tag_DEVICE, opt_dst);
      break;

    default:
      send_sym(tag_CONNSTATE, st_DISCONNECTED);
      break;
  }

  send_uint(tag_MTU, opt_mtu);
  send_str(tag_SEC_LEVEL, opt_sec_level);
  resp_end();
}

static void set_state(enum state st)
{
	conn_state = st;
        cmd_status(0,0, NULL);
}

static void events_handler(const uint8_t *pdu, uint16_t len, gpointer user_data)
{
	uint8_t *opdu;
	uint8_t evt;
	uint16_t handle, olen;
	size_t plen;

	evt = pdu[0];

	if ( evt != ATT_OP_HANDLE_NOTIFY && evt != ATT_OP_HANDLE_IND )
	{
		printf("#Invalid opcode %02X in event handler??\n", evt);
		return;
	}

	assert( len >= 3 );
	handle = att_get_u16(&pdu[1]);

	resp_begin( evt==ATT_OP_HANDLE_NOTIFY ? rsp_NOTIFY : rsp_IND );
	send_uint( tag_HANDLE, handle );
	send_data( pdu+3, len-3 );
	resp_end();

	if (evt == ATT_OP_HANDLE_NOTIFY)
		return;

	opdu = g_attrib_get_buffer(attrib, &plen);
	olen = enc_confirmation(opdu, plen);

	if (olen > 0)
		g_attrib_send(attrib, 0, opdu, olen, NULL, NULL, NULL);
}

static void gatts_find_info_req(const uint8_t *pdu, uint16_t len, gpointer user_data)
{
	uint8_t *opdu;
	uint8_t opcode;
	uint16_t starting_handle , olen;
	size_t plen;

	assert( len == 5 );
	opcode = pdu[0];
	starting_handle = att_get_u16(&pdu[1]);

	opdu = g_attrib_get_buffer(attrib, &plen);
	olen = enc_error_resp(opcode, starting_handle, ATT_ECODE_REQ_NOT_SUPP, opdu, plen);
	if (olen > 0)
		g_attrib_send(attrib, 0, opdu, olen, NULL, NULL, NULL);
}

static void gatts_find_by_type_req(const uint8_t *pdu, uint16_t len, gpointer user_data)
{
	uint8_t *opdu;
	uint8_t opcode;
	uint16_t starting_handle, olen;
	size_t plen;

	assert( len >= 7 );
	opcode = pdu[0];
	starting_handle = att_get_u16(&pdu[1]);

	opdu = g_attrib_get_buffer(attrib, &plen);
	olen = enc_error_resp(opcode, starting_handle, ATT_ECODE_REQ_NOT_SUPP, opdu, plen);
	if (olen > 0)
		g_attrib_send(attrib, 0, opdu, olen, NULL, NULL, NULL);
}

static void gatts_read_by_type_req(const uint8_t *pdu, uint16_t len, gpointer user_data)
{
	uint8_t *opdu;
	uint8_t opcode;
	uint16_t starting_handle, olen;
	size_t plen;

	assert( len == 7 || len == 21 );
	opcode = pdu[0];
	starting_handle = att_get_u16(&pdu[1]);

	opdu = g_attrib_get_buffer(attrib, &plen);
	olen = enc_error_resp(opcode, starting_handle, ATT_ECODE_REQ_NOT_SUPP, opdu, plen);
	if (olen > 0)
		g_attrib_send(attrib, 0, opdu, olen, NULL, NULL, NULL);
}

static void gatts_read_req(const uint8_t *pdu, uint16_t len, gpointer user_data)
{
	uint8_t *opdu;
	uint8_t opcode;
	uint16_t handle, olen;
	size_t plen;

	assert( len == 3 );
	opcode = pdu[0];
	handle = att_get_u16(&pdu[1]);

	opdu = g_attrib_get_buffer(attrib, &plen);
	olen = enc_error_resp(opcode, handle, ATT_ECODE_REQ_NOT_SUPP, opdu, plen);
	if (olen > 0)
		g_attrib_send(attrib, 0, opdu, olen, NULL, NULL, NULL);
}

static void gatts_read_blob_req(const uint8_t *pdu, uint16_t len, gpointer user_data)
{
	uint8_t *opdu;
	uint8_t opcode;
	uint16_t handle, olen;
	size_t plen;

	assert( len == 5 );
	opcode = pdu[0];
	handle = att_get_u16(&pdu[1]);

	opdu = g_attrib_get_buffer(attrib, &plen);
	olen = enc_error_resp(opcode, handle, ATT_ECODE_REQ_NOT_SUPP, opdu, plen);
	if (olen > 0)
		g_attrib_send(attrib, 0, opdu, olen, NULL, NULL, NULL);
}

static void gatts_read_multi_req(const uint8_t *pdu, uint16_t len, gpointer user_data)
{
	uint8_t *opdu;
	uint8_t opcode;
	uint16_t handle1, olen; //offset;
	size_t plen;

	assert( len >= 5 );
	opcode = pdu[0];
	handle1 = att_get_u16(&pdu[1]);

	opdu = g_attrib_get_buffer(attrib, &plen);
	olen = enc_error_resp(opcode, handle1, ATT_ECODE_REQ_NOT_SUPP, opdu, plen);
	if (olen > 0)
		g_attrib_send(attrib, 0, opdu, olen, NULL, NULL, NULL);
}

static void gatts_read_by_group_req(const uint8_t *pdu, uint16_t len, gpointer user_data)
{
	uint8_t *opdu;
	uint8_t opcode;
	uint16_t starting_handle, olen;
	size_t plen;

	assert( len >= 7 );
	opcode = pdu[0];
	starting_handle = att_get_u16(&pdu[1]);

	opdu = g_attrib_get_buffer(attrib, &plen);
	olen = enc_error_resp(opcode, starting_handle, ATT_ECODE_REQ_NOT_SUPP, opdu, plen);
	if (olen > 0)
		g_attrib_send(attrib, 0, opdu, olen, NULL, NULL, NULL);
}

static void gatts_write_req(const uint8_t *pdu, uint16_t len, gpointer user_data)
{
	uint8_t *opdu;
	uint8_t opcode;
	uint16_t handle, olen;
	size_t plen;

	assert( len >= 3 );
	opcode = pdu[0];
	handle = att_get_u16(&pdu[1]);

	opdu = g_attrib_get_buffer(attrib, &plen);
	olen = enc_error_resp(opcode, handle, ATT_ECODE_REQ_NOT_SUPP, opdu, plen);
	if (olen > 0)
		g_attrib_send(attrib, 0, opdu, olen, NULL, NULL, NULL);
}

static void gatts_write_cmd(const uint8_t *pdu, uint16_t len, gpointer user_data)
{
	uint8_t opcode;
	uint16_t handle;

	assert( len >= 3 );
	opcode = pdu[0];
	handle = att_get_u16(&pdu[1]);
}

static void gatts_signed_write_cmd(const uint8_t *pdu, uint16_t len, gpointer user_data)
{
	uint8_t opcode;
	uint16_t handle;

	assert( len >= 15 );
	opcode = pdu[0];
	handle = att_get_u16(&pdu[1]);
}

static void gatts_prep_write_req(const uint8_t *pdu, uint16_t len, gpointer user_data)
{
	uint8_t *opdu;
	uint8_t opcode;
	uint16_t handle, olen;
	size_t plen;

	assert( len >= 5 );
	opcode = pdu[0];
	handle = att_get_u16(&pdu[1]);

	opdu = g_attrib_get_buffer(attrib, &plen);
	olen = enc_error_resp(opcode, handle, ATT_ECODE_REQ_NOT_SUPP, opdu, plen);
	if (olen > 0)
		g_attrib_send(attrib, 0, opdu, olen, NULL, NULL, NULL);
}

static void gatts_exec_write_req(const uint8_t *pdu, uint16_t len, gpointer user_data)
{
	uint8_t *opdu;
	uint8_t opcode,flags;
	uint16_t olen;
	size_t plen;

	assert( len == 5 );
	opcode = pdu[0];
	flags = pdu[1];

	opdu = g_attrib_get_buffer(attrib, &plen);
	olen = enc_error_resp(opcode, 0, ATT_ECODE_REQ_NOT_SUPP, opdu, plen);
	if (olen > 0)
		g_attrib_send(attrib, 0, opdu, olen, NULL, NULL, NULL);
}

static void connect_cb(GIOChannel *io, GError *err, gpointer user_data)
{
	uint16_t mtu;
	uint16_t cid;
	if (err) {
		set_state(STATE_DISCONNECTED);
		resp_error(err_CONN_FAIL);
		printf("# Connect error: %s\n", err->message);
		return;
	}
	bt_io_get(io, &err, BT_IO_OPT_IMTU, &mtu,
                BT_IO_OPT_CID, &cid, BT_IO_OPT_INVALID);

	attrib = g_attrib_new(io,mtu);
	g_attrib_register(attrib, ATT_OP_HANDLE_NOTIFY, GATTRIB_ALL_HANDLES,
						events_handler, attrib, NULL);
	g_attrib_register(attrib, ATT_OP_HANDLE_IND, GATTRIB_ALL_HANDLES,
						events_handler, attrib, NULL);
	g_attrib_register(attrib, ATT_OP_FIND_INFO_REQ, GATTRIB_ALL_HANDLES,
	                  gatts_find_info_req, attrib, NULL);
	g_attrib_register(attrib, ATT_OP_FIND_BY_TYPE_REQ, GATTRIB_ALL_HANDLES,
	                  gatts_find_by_type_req, attrib, NULL);
	g_attrib_register(attrib, ATT_OP_READ_BY_TYPE_REQ, GATTRIB_ALL_HANDLES,
	                  gatts_read_by_type_req, attrib, NULL);
	g_attrib_register(attrib, ATT_OP_READ_REQ, GATTRIB_ALL_HANDLES,
	                  gatts_read_req, attrib, NULL);
	g_attrib_register(attrib, ATT_OP_READ_BLOB_REQ, GATTRIB_ALL_HANDLES,
	                  gatts_read_blob_req, attrib, NULL);
	g_attrib_register(attrib, ATT_OP_READ_MULTI_REQ, GATTRIB_ALL_HANDLES,
	                  gatts_read_multi_req, attrib, NULL);
	g_attrib_register(attrib, ATT_OP_READ_BY_GROUP_REQ, GATTRIB_ALL_HANDLES,
	                  gatts_read_by_group_req, attrib, NULL);
	g_attrib_register(attrib, ATT_OP_WRITE_REQ, GATTRIB_ALL_HANDLES,
	                  gatts_write_req, attrib, NULL);
	g_attrib_register(attrib, ATT_OP_WRITE_CMD, GATTRIB_ALL_HANDLES,
	                  gatts_write_cmd, attrib, NULL);
	g_attrib_register(attrib, ATT_OP_SIGNED_WRITE_CMD, GATTRIB_ALL_HANDLES,
	                  gatts_signed_write_cmd, attrib, NULL);
	g_attrib_register(attrib, ATT_OP_PREP_WRITE_REQ, GATTRIB_ALL_HANDLES,
	                  gatts_prep_write_req, attrib, NULL);
	g_attrib_register(attrib, ATT_OP_EXEC_WRITE_REQ, GATTRIB_ALL_HANDLES,
	                  gatts_exec_write_req, attrib, NULL);

	set_state(STATE_CONNECTED);
	operation(attrib);
}

static void disconnect_io()
{
	if (conn_state == STATE_DISCONNECTED)
		return;

	g_attrib_unref(attrib);
	attrib = NULL;
	opt_mtu = 0;

	g_io_channel_shutdown(iochannel, FALSE, NULL);
	g_io_channel_unref(iochannel);
	iochannel = NULL;

	set_state(STATE_DISCONNECTED);
}
static int read_flags(uint8_t *flags, const uint8_t *data, size_t size)
{
	size_t offset;

	if (!flags || !data)
		return -EINVAL;

	offset = 0;
	while (offset < size) {
		uint8_t len = data[offset];
		uint8_t type;

		/* Check if it is the end of the significant part */
		if (len == 0)
			break;

		if (len + offset > size)
			break;

		type = data[offset + 1];

		if (type == FLAGS_AD_TYPE) {
			*flags = data[offset + 2];
			return 0;
		}

		offset += 1 + len;
	}

	return -ENOENT;
}
static int check_report_filter(uint8_t procedure, le_advertising_info *info)
{
	uint8_t flags;

	/* If no discovery procedure is set, all reports are treat as valid */
	if (procedure == 0)
		return 1;

	/* Read flags AD type value from the advertising report if it exists */
	if (read_flags(&flags, info->data, info->length))
		return 0;

	switch (procedure) {
	case 'l': /* Limited Discovery Procedure */
		if (flags & FLAGS_LIMITED_MODE_BIT)
			return 1;
		break;
	case 'g': /* General Discovery Procedure */
		if (flags & (FLAGS_LIMITED_MODE_BIT | FLAGS_GENERAL_MODE_BIT))
			return 1;
		break;
	default:
		fprintf(stderr, "Unknown discovery procedure\n");
	}

	return 0;
}

static void sigint_handler(int sig)
{
	signal_received = sig;
}

struct le_devices eir_parse_name(uint8_t *eir, size_t eir_len,
						char *buf, size_t buf_len)
{
	size_t offset;
	struct le_devices devices;
	offset = 0;
	while (offset < eir_len) {
		uint8_t field_len = eir[0];
		size_t name_len;

		/* Check for the end of EIR */
		if (field_len == 0)
			break;

		if (offset + field_len > eir_len)
			goto failed;

		switch (eir[1]) {
		case EIR_NAME_SHORT:
		case EIR_NAME_COMPLETE:
		{
			name_len = field_len - 1;
			if (name_len > buf_len)
				goto failed;

			memcpy(buf, &eir[2], name_len);
			int i;
		    for(i=1; i<field_len; i++)
		    {
		      printf("\tData: 0x%0X\n", eir[i]);
		    }
			break;
		}
		case EIR_FLAGS:
		{
			printf("Flag type: len=%02X\n", field_len);
		    int i;
		    for(i=1; i<field_len; i++)
		    {
		      printf("\tFlag data: 0x%0X\n", eir[i]);
		    }
		    break;
		}
		case EIR_UUID16_SOME:
		case EIR_UUID16_ALL:
		case EIR_UUID32_SOME:
		case EIR_TX_POWER:
		case EIR_APPERANCE:
		case EIR_SLAVE_CONN_INTVAL:
		case EIR_UUID128_ALL:
		case EIR_MANUFACTURE_SPECIFIC:
		{
			printf("type: %02X len: %02X \n",eir[1],field_len);
		    int i;
		    for(i=2; i<field_len; i++)
		    {
		      printf("\tData: 0x%0X\n", eir[i]);
		    }
		    if(EIR_MANUFACTURE_SPECIFIC)
		    {
			    if( (eir[3] << 8 | eir[2]) == MANU_TYPE )
			    {
			    	devices.manufacturer = MANU_TYPE;
			    	devices.status = eir[field_len];
			    	devices.type = eir[field_len-1];
			    }
			}
		    break;
		}

		}

		offset += field_len + 1;
		eir += field_len + 1;
	}

failed:
	snprintf(buf, buf_len, "(unknown)");
	return devices;
}

void check_configure(int devices_type, int devices_status)
{
	printf("check configure \n");
	printf("Type: %02X \n",devices_type);

	switch(devices_status)
	{
		case 0x00:
			{
				printf("unconfigure \n");
			break;
			}
		case 0x001:
			{
				printf("configured \n");
				break;
			}
		default:
			{
				printf("reserved\n");
				break;
			}

	}
}
static gboolean channel_watcher(GIOChannel *chan, GIOCondition cond,
				gpointer user_data)
{
	if(chan == iochannel)
	disconnect_io();

	return FALSE;
}
static void le_connect(gpointer user_data )
{
	GError *gerr = NULL;

	if (opt_dst == NULL) {
		error("Remote Bluetooth address required\n");
		resp_error(err_BAD_PARAM);
		return;
	}
	set_state(STATE_CONNECTING);
	iochannel = gatt_connect(opt_src, opt_dst, opt_dst_type, opt_sec_level,
						opt_psm, opt_mtu, connect_cb,&gerr);

	if (iochannel == NULL)
		set_state(STATE_DISCONNECTED);

	else
		g_io_add_watch(iochannel, G_IO_HUP, channel_watcher, NULL);
}

static int print_advertising_devices(int dd, uint8_t filter_type)
{
	
	unsigned char buf[HCI_MAX_EVENT_SIZE], *ptr;
	struct hci_filter nf, of;
	struct sigaction sa;
	socklen_t olen;
	int len,to=5000;

	event_loop = g_main_loop_new(NULL,FALSE);

	olen = sizeof(of);
	if (getsockopt(dd, SOL_HCI, HCI_FILTER, &of, &olen) < 0) {
		printf("Could not get socket options\n");
		return -1;
	}

	hci_filter_clear(&nf);
	hci_filter_set_ptype(HCI_EVENT_PKT, &nf);
	hci_filter_set_event(EVT_LE_META_EVENT, &nf);

	if (setsockopt(dd, SOL_HCI, HCI_FILTER, &nf, sizeof(nf)) < 0) {
		printf("Could not set socket options\n");
		return -1;
	}

	memset(&sa, 0, sizeof(sa));
	sa.sa_flags = SA_NOCLDSTOP;
	sa.sa_handler = sigint_handler;
	sigaction(SIGINT, &sa, NULL);
	
	while(1)
	{
		evt_le_meta_event *meta;
		le_advertising_info *info;
		char addr[18];

		 if (to) {
              struct pollfd p;
             int n;
 
             p.fd = dd; p.events = POLLIN;
             while ((n = poll(&p, 1, to)) < 0) {
                 if (errno == EAGAIN || errno == EINTR)
                     continue;
                 goto done;
             }
 
             if (!n) {
                 errno = ETIMEDOUT;
                 goto done;
             }

             to -= 10;
             if (to < 0)
                 to = 0;
         } 

	while ((len = read(dd, buf, sizeof(buf))) < 0)
		{
			if (errno == EINTR && signal_received == SIGINT) {
				len = 0;
				goto done;
			}
			if (errno == EAGAIN || errno == EINTR)
				continue;
			goto done;

		}

		ptr = buf + (1 + HCI_EVENT_HDR_SIZE);
		len -= (1 + HCI_EVENT_HDR_SIZE);

		meta = (void *) ptr;

		if (meta->subevent != 0x02)
			goto done;

		/* Ignoring multiple reports */
		info = (le_advertising_info *) (meta->data + 1);

		if (check_report_filter(filter_type, info)) {
			char name[30];
			//memset(name, 0, sizeof(name));
			memset(name, 0, sizeof(name));
			//ba2str(&info->bdaddr, addr);
			le_devices.bdaddr = info->bdaddr;
			ba2str(&le_devices.bdaddr, addr);
			//strcpy(le_devices->name,name);

			le_devices = eir_parse_name(info->data, info->length,
							name, sizeof(name) - 1);

			printf("%s %s\n", addr,name);
			if(le_devices.manufacturer == MANU_TYPE)
			{
				if(le_devices.status == 0x00)
				{
					flag_connect = 1;
					printf("le_devices.manufacture: %02X \n",le_devices.manufacturer);
					check_configure(le_devices.type,le_devices.status);
					opt_dst = g_strdup(addr);
					goto done;
				}
			}
			printf("--------\n");
		}
	}
done:
	setsockopt(dd, SOL_HCI, HCI_FILTER, &of, sizeof(of));

	if (len < 0)
		return -1;

	return 0;
 	
}

static const char *lescan_help =
	"Usage:\n"
	"\tlescan [--privacy] enable privacy\n"
	"\tlescan [--passive] set scan type passive (default active)\n"
	"\tlescan [--whitelist] scan for address in the whitelist only\n"
	"\tlescan [--discovery=g|l] enable general or limited discovery"
		"procedure\n"
	"\tlescan [--duplicates] don't filter duplicates\n";

static struct option lescan_options[] = {
	{ "help",	0, 0, 'h' },
	{ "privacy",	0, 0, 'p' },
	{ "passive",	0, 0, 'P' },
	{ "whitelist",	0, 0, 'w' },
	{ "discovery",	1, 0, 'd' },
	{ "duplicates",	0, 0, 'D' },
	{ 0, 0, 0, 0 }
};
static void helper_arg(int min_num_arg, int max_num_arg, int *argc,
			char ***argv, const char *usage)
{
	*argc -= optind;
	/* too many arguments, but when "max_num_arg < min_num_arg" then no
		 limiting (prefer "max_num_arg=-1" to gen infinity)
	*/
	if ( (*argc > max_num_arg) && (max_num_arg >= min_num_arg ) ) 
	{
		fprintf(stderr, "%s: too many arguments (maximal: %i)\n",
				*argv[1], max_num_arg);
		printf("%s", usage);
		exit(1);
	}

	/* print usage */
	if (*argc < min_num_arg) 
	{
		fprintf(stderr, "%s: too few arguments (minimal: %i)\n",
				*argv[1], min_num_arg);
		printf("%s", usage);
		exit(0);
	}

	*argv += optind;
}

static void * lescan_bt_devices(int dev_id, int argc, char **argv)
{
	int err,opt, dd;
	uint8_t own_type = 0x00;
	uint8_t scan_type = 0x01;
	uint8_t filter_type = 0;
	uint8_t filter_policy = 0x00;
	uint16_t interval = htobs(0x0010);
	uint16_t window = htobs(0x0010);
	uint8_t filter_dup = 1;
	
	for_each_opt(opt, lescan_options, NULL) {
		switch (opt) {
		case 'p':
			own_type = 0x01; /* Random */
			break;
		case 'P':
			scan_type = 0x00; /* Passive */
			break;
		case 'w':
			filter_policy = 0x01; /* Whitelist */
			break;
		case 'd':
			filter_type = optarg[0];
			if (filter_type != 'g' && filter_type != 'l') {
				fprintf(stderr, "Unknown discovery procedure\n");
				exit(1);
			}

			interval = htobs(0x0012);
			window = htobs(0x0012);
			break;
		case 'D':
			filter_dup = 0x00;
			break;
		default:
			printf("%s", lescan_help);
			
		}
	}

	dev_id = hci_get_route(NULL);
    dd = hci_open_dev( dev_id );
    if (dev_id < 0 || dd < 0)
    {
        perror("opening socket");
        exit(1);
    }
	err = hci_le_set_scan_parameters(dd, scan_type, interval, window,
						own_type, filter_policy, 1000);
	if (err < 0) {
		perror("Set scan parameters failed");
		exit(1);
	}

	err = hci_le_set_scan_enable(dd, 0x01, filter_dup, 1000);
	if (err < 0) {
		perror("Enable scan failed");
		exit(1);
	}

	printf("LE Scan ...\n");

	err = print_advertising_devices(dd, filter_type);

	if (err < 0) {
		perror("Could not receive advertising events");
		exit(1);
	}

	err = hci_le_set_scan_enable(dd, 0x00, filter_dup, 1000);

	if (err < 0) {
		perror("Disable scan failed");
		exit(1);
	}
	printf("LE Scan finish ! \n");
	hci_close_dev(dd);
	printf("opt_dst: %s\n",opt_dst);
	if(flag_connect == 1)
	{
	le_connect(user_data);	
	
	operation = char_write_auto;
	g_main_loop_run(event_loop);
	}

}

static void cmd_lescan (int dev_id,int argc ,char **argvp)
{
	lescan_bt_devices(dev_id,argc,argvp);
}

static void char_write_req_cb(guint8 status, const guint8 *pdu, guint16 plen,
							gpointer user_data)
{
	if (status != 0) {
		resp_error(err_COMM_ERR); // Todo: status
		goto done;
	}

	if (!dec_write_resp(pdu, plen) && !dec_exec_write_resp(pdu, plen)) {
		resp_error(err_PROTO_ERR);
		goto done;
	}

        resp_begin(rsp_WRITE);
        resp_end();
done:
	if (opt_listen == FALSE)
		g_main_loop_quit(event_loop);
}

static gboolean char_write_auto(gpointer user_data)
{
	GAttrib *attrib = user_data;
	uint8_t *value ;
	size_t len;
	char *str_value = "68656c6c6f";

	len = gatt_attr_data_from_string(str_value, &value);
	if (len == 0) {
		g_printerr("Invalid value\n");
		goto error;
	}
	gatt_write_char(attrib, CHARACTERISTICS_HANDLE, value, len, char_write_req_cb,
									NULL);
	return FALSE ;

error:
	g_main_loop_quit(event_loop);
	return FALSE ;
}

enum ENUM_COMMAND{
	ENUM_COMMAND_LESCAN = 0,
	ENUM_END
};

static struct {
	char *cmd;
	void (*func)(int parameter, int argcp, char **argvp);
	char *params;
	char *desc;
} commands[] = {
	{ "lescan",			cmd_lescan,			"s", 			"Scan LE devices" },
	{ NULL, NULL, 0}
};
static GOptionEntry bt_options[] = {
	{ NULL },
};

int main(int argc, char *argv[])
{
	GOptionContext *context;
	GOptionGroup *bt_group;
	GError *gerr = NULL;
	GIOChannel *pchan;

	opt_sec_level = g_strdup("low");
	opt_dst_type = g_strdup("public");

	char *argvp;
    argvp = *argv ;

    context = g_option_context_new(NULL);
	g_option_context_add_main_entries(context, bt_options, NULL);

	bt_group = g_option_group_new("char-read-write",
		"Characteristics Value/Descriptor Read/Write arguments",
		"Show all Characteristics Value/Descriptor Read/Write "
		"arguments",
		NULL, NULL);

	g_option_context_add_group(context, bt_group);
	g_option_group_add_entries(bt_group, bt_options);
	

	if (g_option_context_parse(context, &argc, &argv, &gerr) == FALSE) 
	{
		g_printerr("%s\n", gerr->message);
		g_error_free(gerr);
	}

	commands[ENUM_COMMAND_LESCAN].func(di.dev_id, argc, argv);


	if (opt_dst == NULL) 
	{
		g_print("Remote Bluetooth address required\n");
		got_error = TRUE;
		goto finish;
	}
	
	pchan = gatt_connect(opt_src, opt_dst, opt_dst_type, opt_sec_level,
					opt_psm, opt_mtu, connect_cb,&gerr);
	if (pchan == NULL) 
	{
		got_error = TRUE;
		goto finish;
	}

	event_loop = g_main_loop_new(NULL, FALSE);
	g_main_loop_run(event_loop);
	g_main_loop_unref(event_loop);

finish:
	g_option_context_free(context);
	g_free(opt_src);
	g_free(opt_dst);
	g_free(opt_sec_level);

	if (got_error)
		exit(EXIT_FAILURE);
	else
		exit(EXIT_SUCCESS);
}
