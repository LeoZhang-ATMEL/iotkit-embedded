/*
 * Copyright (c) 2014-2016 Alibaba Group. All rights reserved.
 * License-Identifier: Apache-2.0
 *
 * Licensed under the Apache License, Version 2.0 (the "License"); you may
 * not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *    http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS, WITHOUT
 * WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 *
 */


#include <stdio.h>

#include "iot_import.h"
#include "iot_export_errno.h"

#include "lite-utils.h"
#include "utils_hmac.h"
#include "json_parser.h"
#include "CoAPMessage.h"
#include "CoAPExport.h"
#include "lite-system.h"

#define IOTX_SIGN_LENGTH         (40+1)
#define IOTX_SIGN_SOURCE_LEN     (256)
#define IOTX_AUTH_TOKEN_LEN      (192+1)
#define IOTX_COAP_INIT_TOKEN     (0x01020304)
#define IOTX_LIST_MAX_ITEM       (10)


#define IOTX_AUTH_STR      "auth"
#define IOTX_SIGN_SRC_STR  "clientId%sdeviceName%sproductKey%s"
#define IOTX_AUTH_DEVICENAME_STR "{\"productKey\":\"%s\",\"deviceName\":\"%s\",\"clientId\":\"%s\",\"sign\":\"%s\"}"

#define IOTX_COAP_ONLINE_DTLS_SERVER_URL "coaps://%s.iot-as-coap.cn-shanghai.aliyuncs.com:5684"


typedef struct {
    char                *p_auth_token;
    int                  auth_token_len;
    char                 is_authed;
    iotx_deviceinfo_t   *p_devinfo;
    CoAPContext         *p_coap_ctx;
    unsigned int         coap_token;
    iotx_event_handle_t  event_handle;
} iotx_coap_t;


int iotx_calc_sign(const char *p_device_secret, const char *p_client_id,
                   const char *p_device_name, const char *p_product_key, char sign[IOTX_SIGN_LENGTH])
{
    char *p_msg = NULL;

    p_msg = (char *)coap_malloc(IOTX_SIGN_SOURCE_LEN);
    if (NULL == p_msg) {
        return IOTX_ERR_NO_MEM;
    }
    memset(sign,  0x00, IOTX_SIGN_LENGTH);
    memset(p_msg, 0x00, IOTX_SIGN_SOURCE_LEN);

    HAL_Snprintf(p_msg, IOTX_SIGN_SOURCE_LEN,
                 IOTX_SIGN_SRC_STR,
                 p_client_id,
                 p_device_name,
                 p_product_key);
    utils_hmac_md5(p_msg, strlen(p_msg), sign, p_device_secret, strlen(p_device_secret));

    coap_free(p_msg);
    COAP_DEBUG("The device name sign: %s", sign);
    return IOTX_SUCCESS;
}

static int iotx_get_token_from_json(char *p_str, char *p_token, int len)
{
    char *p_value = NULL;
    if (NULL == p_str || NULL == p_token) {
        COAP_ERR("Invalid paramter p_str %p, p_token %p", p_str, p_token);
        return IOTX_ERR_INVALID_PARAM;
    }

    p_value = LITE_json_value_of("token", p_str);
    if (NULL != p_value) {
        if (len - 1 < strlen(p_value)) {
            return IOTX_ERR_BUFF_TOO_SHORT;
        }
        memset(p_token, 0x00, len);
        strncpy(p_token, p_value, strlen(p_value));
        LITE_free(p_value);
        return IOTX_SUCCESS;
    }

    return IOTX_ERR_AUTH_FAILED;
}

static void iotx_device_name_auth_callback(void *user, void *p_message)
{
    int ret_code = IOTX_SUCCESS;
    iotx_coap_t *p_iotx_coap = NULL;
    CoAPMessage *message = (CoAPMessage *)p_message;

    if (NULL == user) {
        COAP_ERR("Invalid paramter, p_arg %p", user);
        return ;
    }
    p_iotx_coap = (iotx_coap_t *)user;

    if (NULL == message) {
        COAP_ERR("Invalid paramter, message %p",  message);
        return;
    }
    COAP_DEBUG("Receive response message:");
    COAP_DEBUG("* Response Code : 0x%x", message->header.code);
    COAP_DEBUG("* Payload: %s", message->payload);

    switch (message->header.code) {
        case COAP_MSG_CODE_205_CONTENT: {
            ret_code = iotx_get_token_from_json((char *)message->payload, p_iotx_coap->p_auth_token, p_iotx_coap->auth_token_len);
            if (IOTX_SUCCESS == ret_code) {
                p_iotx_coap->is_authed = IOT_TRUE;
                COAP_INFO("CoAP authenticate success!!!");
            }
            break;
        }
        case COAP_MSG_CODE_500_INTERNAL_SERVER_ERROR: {
            COAP_INFO("CoAP internal server error, authenticate failed, will retry it");
            HAL_SleepMs(1000);
            IOT_CoAP_DeviceNameAuth((iotx_coap_context_t *)p_iotx_coap);
            break;
        }
        default:
            break;
    }

}

static unsigned int iotx_get_coap_token(iotx_coap_t       *p_iotx_coap, unsigned char *p_encoded_data)
{
    unsigned int value = p_iotx_coap->coap_token;
    p_encoded_data[0] = (unsigned char)((value & 0x00FF) >> 0);
    p_encoded_data[1] = (unsigned char)((value & 0xFF00) >> 8);
    p_encoded_data[2] = (unsigned char)((value & 0xFF0000) >> 16);
    p_encoded_data[3] = (unsigned char)((value & 0xFF000000) >> 24);
    p_iotx_coap->coap_token++;
    return sizeof(unsigned int);
}

void iotx_event_notifyer(unsigned int code, CoAPMessage *message)
{
    if (NULL == message) {
        COAP_ERR("Invalid paramter, message %p", message);
        return ;
    }

    COAP_DEBUG("Error code: 0x%x, payload: %s", code, message->payload);
    switch (code) {
        case COAP_MSG_CODE_402_BAD_OPTION:
        case COAP_MSG_CODE_401_UNAUTHORIZED: {
            iotx_coap_t *p_context = NULL;
            if (NULL != message->user) {
                p_context = (iotx_coap_t *)message->user;
                p_context->is_authed = IOT_FALSE;
                IOT_CoAP_DeviceNameAuth(p_context);
                COAP_INFO("IoTx token expired, will reauthenticate");
            }
            /* TODO: call event handle to notify application */
            /* p_context->event_handle(); */
            break;
        }

        default:
            break;
    }
}

static void iotx_get_well_known_handler(void *arg, void *p_response)
{

    int            len       = 0;
    unsigned char *p_payload = NULL;
    iotx_coap_resp_code_t resp_code;
    IOT_CoAP_GetMessageCode(p_response, &resp_code);
    IOT_CoAP_GetMessagePayload(p_response, &p_payload, &len);
    COAP_INFO("[APPL]: Message response code: %d", resp_code);
    COAP_INFO("[APPL]: Len: %d, Payload: %s, ", len, p_payload);
}


int iotx_get_well_known(iotx_coap_context_t *p_context)
{
    int len = 0;
    CoAPContext      *p_coap_ctx = NULL;
    iotx_coap_t      *p_iotx_coap = NULL;
    CoAPMessage      message;
    unsigned char    token[8] = {0};

    p_iotx_coap = (iotx_coap_t *)p_context;
    p_coap_ctx = (CoAPContext *)p_iotx_coap->p_coap_ctx;


    CoAPMessage_init(&message);
    CoAPMessageType_set(&message, COAP_MESSAGE_TYPE_CON);
    CoAPMessageCode_set(&message, COAP_MSG_CODE_GET);
    CoAPMessageId_set(&message, CoAPMessageId_gen(p_coap_ctx));
    len = iotx_get_coap_token(p_iotx_coap, token);
    CoAPMessageToken_set(&message, token, len);
    CoAPMessageHandler_set(&message, iotx_get_well_known_handler);
    CoAPStrOption_add(&message, COAP_OPTION_URI_PATH, (unsigned char *)".well-known", strlen(".well-known"));
    CoAPStrOption_add(&message, COAP_OPTION_URI_PATH, (unsigned char *)"core", strlen("core"));
    CoAPUintOption_add(&message, COAP_OPTION_ACCEPT, COAP_CT_APP_LINK_FORMAT);
    CoAPMessageUserData_set(&message, (void *)p_iotx_coap);
    CoAPMessage_send(p_coap_ctx, &message);
    CoAPMessage_destory(&message);
    return IOTX_SUCCESS;
}

static void iotx_coap_mid_rsphdl(void *arg, void *p_response)
{
    int                     p_payload_len = 0;
    unsigned char          *p_payload = NULL;
    char                   *msg = NULL;
    iotx_coap_resp_code_t   resp_code;

    IOT_CoAP_GetMessageCode(p_response, &resp_code);
    IOT_CoAP_GetMessagePayload(p_response, &p_payload, &p_payload_len);
    log_debug("MID Report: CoAP response code = %d", resp_code);
    log_debug("MID Report: CoAP msg_len = %d", p_payload_len);
    if (p_payload_len > 0) {
        log_debug("MID Report: CoAP msg = '%s'", p_payload);
        msg = json_get_value_by_name((char *)p_payload, p_payload_len, "id", &p_payload_len, 0);
        if (NULL != msg) {
            log_debug("MID Report: CoAP mid_report responseID = '%s'", msg);
        } else {
            log_warning("MID Report: CoAP mid_report responseID not found in msg");
        }
    } else {
        log_warning("MID Report: CoAP response payload_len = 0");
    }
}

/* report ModuleID */
static int iotx_coap_report_mid(iotx_coap_context_t *p_context)
{
    int                     ret;
    char                    topic_name[IOTX_URI_MAX_LEN + 1];
    iotx_message_t          message;
    char                    requestId[MIDREPORT_REQID_LEN + 1] = {0};
    iotx_coap_t            *p_iotx_coap = (iotx_coap_t *)p_context;
    char                    pid[PID_STRLEN_MAX + 1] = {0};
    char                    mid[MID_STRLEN_MAX + 1] = {0};
    CoAPContext            *p_coap_ctx = NULL;

    memset(pid, 0, sizeof(pid));
    memset(mid, 0, sizeof(mid));

    if (0 == HAL_GetPartnerID(pid)) {
        log_debug("PartnerID is Null");
        return SUCCESS_RETURN;
    }
    if (0 == HAL_GetModuleID(mid)) {
        log_debug("ModuleID is Null");
        return SUCCESS_RETURN;
    }
    if (NULL == p_iotx_coap) {
        log_err("Invalid param: p_context is NULL");
        return FAIL_RETURN;
    }

    log_debug("MID Report: started in CoAP");
    p_coap_ctx = (CoAPContext *)p_iotx_coap->p_coap_ctx;

    iotx_midreport_reqid(requestId,
                         p_iotx_coap->p_devinfo->product_key,
                         p_iotx_coap->p_devinfo->device_name);
    /* 1,generate json data */
    char *msg = HAL_Malloc(MIDREPORT_PAYLOAD_LEN);
    if (NULL == msg) {
        log_err("allocate mem failed");
        return FAIL_RETURN;
    }

    iotx_midreport_payload(msg,
                           requestId,
                           mid,
                           pid);

    log_debug("MID Report: json data = '%s'", msg);

    memset(&message, 0, sizeof(iotx_message_t));

    message.p_payload = (unsigned char *)msg;
    message.payload_len = (unsigned short)strlen(msg);
    message.resp_callback = iotx_coap_mid_rsphdl;
    message.msg_type = IOTX_MESSAGE_NON;
    message.content_type = IOTX_CONTENT_TYPE_JSON;

    /* 2,generate topic name */
    ret = iotx_midreport_topic(topic_name,
                               "/topic",
                               p_iotx_coap->p_devinfo->product_key,
                               p_iotx_coap->p_devinfo->device_name);

    log_debug("MID Report: topic name = '%s'", topic_name);

    if (ret < 0) {
        log_err("generate topic name of info failed");
        HAL_Free(msg);
        return FAIL_RETURN;
    }

    if (IOTX_SUCCESS != (ret = IOT_CoAP_SendMessage(p_context, topic_name, &message))) {
        log_err("send CoAP msg failed, ret = %d", ret);
        HAL_Free(msg);
        return FAIL_RETURN;
    }
    HAL_Free(msg);
    log_debug("MID Report: IOT_CoAP_SendMessage() = %d", ret);

    ret = CoAPMessage_recv(p_coap_ctx, CONFIG_COAP_AUTH_TIMEOUT, 1);
    log_debug("MID Report: finished, ret = CoAPMessage_recv() = %d", ret);

    return SUCCESS_RETURN;
}

int IOT_CoAP_DeviceNameAuth(iotx_coap_context_t *p_context)
{
    int len = 0;
    int ret = COAP_SUCCESS;
    CoAPContext      *p_coap_ctx = NULL;
    iotx_coap_t      *p_iotx_coap = NULL;
    CoAPMessage       message;
    unsigned char    *p_payload   = NULL;
    unsigned char     token[8] = {0};
    char sign[IOTX_SIGN_LENGTH]   = {0};

    p_iotx_coap = (iotx_coap_t *)p_context;
    if (NULL == p_iotx_coap || (NULL != p_iotx_coap && (NULL == p_iotx_coap->p_auth_token
                                || NULL == p_iotx_coap->p_coap_ctx || 0 == p_iotx_coap->auth_token_len))) {
        COAP_DEBUG("Invalid paramter");
        return IOTX_ERR_INVALID_PARAM;
    }

    p_coap_ctx = (CoAPContext *)p_iotx_coap->p_coap_ctx;

    CoAPMessage_init(&message);
    CoAPMessageType_set(&message, COAP_MESSAGE_TYPE_CON);
    CoAPMessageCode_set(&message, COAP_MSG_CODE_POST);
    CoAPMessageId_set(&message, CoAPMessageId_gen(p_coap_ctx));
    len = iotx_get_coap_token(p_iotx_coap, token);
    CoAPMessageToken_set(&message, token, len);
    CoAPMessageHandler_set(&message, iotx_device_name_auth_callback);

    CoAPStrOption_add(&message, COAP_OPTION_URI_PATH, (unsigned char *)IOTX_AUTH_STR, strlen(IOTX_AUTH_STR));
    CoAPUintOption_add(&message, COAP_OPTION_CONTENT_FORMAT, COAP_CT_APP_JSON);
    CoAPUintOption_add(&message, COAP_OPTION_ACCEPT, COAP_CT_APP_JSON);
    CoAPMessageUserData_set(&message, (void *)p_iotx_coap);

    p_payload = coap_malloc(COAP_MSG_MAX_PDU_LEN);
    if (NULL == p_payload) {
        CoAPMessage_destory(&message);
        return IOTX_ERR_NO_MEM;
    }
    iotx_calc_sign(p_iotx_coap->p_devinfo->device_secret, p_iotx_coap->p_devinfo->device_id,
                   p_iotx_coap->p_devinfo->device_name, p_iotx_coap->p_devinfo->product_key, sign);
    HAL_Snprintf((char *)p_payload, COAP_MSG_MAX_PDU_LEN,
                 IOTX_AUTH_DEVICENAME_STR,
                 p_iotx_coap->p_devinfo->product_key,
                 p_iotx_coap->p_devinfo->device_name,
                 p_iotx_coap->p_devinfo->device_id,
                 sign);
    CoAPMessagePayload_set(&message, p_payload, strlen((char *)p_payload));
    COAP_DEBUG("The payload is: %p", message.payload);
    COAP_DEBUG("Send authentication message to server");
    ret = CoAPMessage_send(p_coap_ctx, &message);
    coap_free(p_payload);
    CoAPMessage_destory(&message);

    if (COAP_SUCCESS != ret) {
        COAP_DEBUG("Send authentication message to server failed ret = %d", ret);
        return IOTX_ERR_SEND_MSG_FAILED;
    }

    ret = CoAPMessage_recv(p_coap_ctx, CONFIG_COAP_AUTH_TIMEOUT, 2);
    if (0 < ret && !p_iotx_coap->is_authed) {
        COAP_INFO("CoAP authenticate failed");
        return IOTX_ERR_AUTH_FAILED;
    }

    /* report module id */
    ret = iotx_coap_report_mid(p_context);
    if (SUCCESS_RETURN != ret) {
        COAP_DEBUG("Send ModuleId message to server(CoAP) failed ret = %d", ret);
        return IOTX_ERR_SEND_MSG_FAILED;
    }

    return IOTX_SUCCESS;
}

static int iotx_split_path_2_option(char *uri, CoAPMessage *message)
{
    char *ptr     = NULL;
    char *pstr    = NULL;
    char  path[COAP_MSG_MAX_PATH_LEN]  = {0};

    if (NULL == uri || NULL == message) {
        COAP_ERR("Invalid paramter p_path %p, p_message %p", uri, message);
        return IOTX_ERR_INVALID_PARAM;
    }
    if (IOTX_URI_MAX_LEN < strlen(uri)) {
        COAP_ERR("The uri length is too loog,len = %d", (int)strlen(uri));
        return IOTX_ERR_URI_TOO_LOOG;
    }
    COAP_DEBUG("The uri is %s", uri);
    ptr = pstr = uri;
    while ('\0' != *ptr) {
        if ('/' == *ptr) {
            if (ptr != pstr) {
                memset(path, 0x00, sizeof(path));
                strncpy(path, pstr, ptr - pstr);
                COAP_DEBUG("path: %s,len=%d", path, (int)(ptr - pstr));
                CoAPStrOption_add(message, COAP_OPTION_URI_PATH,
                                  (unsigned char *)path, (int)strlen(path));
            }
            pstr = ptr + 1;

        }
        if ('\0' == *(ptr + 1) && '\0' != *pstr) {
            memset(path, 0x00, sizeof(path));
            strncpy(path, pstr, sizeof(path) - 1);
            COAP_DEBUG("path: %s,len=%d", path, (int)strlen(path));
            CoAPStrOption_add(message, COAP_OPTION_URI_PATH,
                              (unsigned char *)path, (int)strlen(path));
        }
        ptr ++;
    }
    return IOTX_SUCCESS;
}

int IOT_CoAP_SendMessage(iotx_coap_context_t *p_context, char *p_path, iotx_message_t *p_message)
{

    int len = 0;
    int ret = IOTX_SUCCESS;
    CoAPContext      *p_coap_ctx = NULL;
    iotx_coap_t      *p_iotx_coap = NULL;
    CoAPMessage      message;
    unsigned char    token[8] = {0};

    p_iotx_coap = (iotx_coap_t *)p_context;

    if (NULL == p_context || NULL == p_path || NULL == p_message ||
        (NULL != p_iotx_coap && NULL == p_iotx_coap->p_coap_ctx)) {
        COAP_ERR("Invalid paramter p_context %p, p_uri %p, p_message %p",
                 p_context, p_path, p_message);
        return IOTX_ERR_INVALID_PARAM;
    }



    if (p_message->payload_len >= COAP_MSG_MAX_PDU_LEN) {
        COAP_ERR("The payload length %d is too loog", p_message->payload_len);
        return IOTX_ERR_MSG_TOO_LOOG;
    }

    p_coap_ctx = (CoAPContext *)p_iotx_coap->p_coap_ctx;
    if (p_iotx_coap->is_authed) {

        CoAPMessage_init(&message);
        CoAPMessageType_set(&message, COAP_MESSAGE_TYPE_CON);
        CoAPMessageCode_set(&message, COAP_MSG_CODE_POST);
        CoAPMessageId_set(&message, CoAPMessageId_gen(p_coap_ctx));
        len = iotx_get_coap_token(p_iotx_coap, token);
        CoAPMessageToken_set(&message, token, len);
        CoAPMessageUserData_set(&message, (void *)p_message->user_data);
        CoAPMessageHandler_set(&message, p_message->resp_callback);

        ret = iotx_split_path_2_option(p_path, &message);
        if (IOTX_SUCCESS != ret) {
            return ret;
        }

        if (IOTX_CONTENT_TYPE_CBOR == p_message->content_type) {
            CoAPUintOption_add(&message, COAP_OPTION_CONTENT_FORMAT, COAP_CT_APP_CBOR);
            CoAPUintOption_add(&message, COAP_OPTION_ACCEPT, COAP_CT_APP_OCTET_STREAM);
        } else {
            CoAPUintOption_add(&message, COAP_OPTION_CONTENT_FORMAT, COAP_CT_APP_JSON);
            CoAPUintOption_add(&message, COAP_OPTION_ACCEPT, COAP_CT_APP_OCTET_STREAM);
        }
        CoAPStrOption_add(&message,  COAP_OPTION_AUTH_TOKEN,
                          (unsigned char *)p_iotx_coap->p_auth_token, strlen(p_iotx_coap->p_auth_token));

        CoAPMessagePayload_set(&message, p_message->p_payload, p_message->payload_len);

        ret = CoAPMessage_send(p_coap_ctx, &message);
        CoAPMessage_destory(&message);
        if (COAP_ERROR_DATA_SIZE == ret) {
            return IOTX_ERR_MSG_TOO_LOOG;
        }
        return IOTX_SUCCESS;
    } else {
        /* COAP_INFO("The client hasn't auth success"); */
        return IOTX_ERR_NOT_AUTHED;
    }
}


int IOT_CoAP_GetMessagePayload(void *p_message, unsigned char **pp_payload, int *p_len)
{
    CoAPMessage *message = NULL;

    if (NULL == p_message || NULL == pp_payload || NULL == p_len) {
        COAP_ERR("Invalid paramter p_message %p, pp_payload %p, p_len %p",
                 p_message, pp_payload, p_len);
        return IOTX_ERR_INVALID_PARAM;
    }
    message = (CoAPMessage *)p_message;
    *pp_payload    =  message->payload;
    *p_len         =  message->payloadlen;

    return IOTX_SUCCESS;
}

int  IOT_CoAP_GetMessageCode(void *p_message, iotx_coap_resp_code_t *p_resp_code)
{
    CoAPMessage *message = NULL;

    if (NULL == p_message || NULL == p_resp_code) {
        COAP_ERR("Invalid paramter p_message %p, p_resp_code %p",
                 p_message, p_resp_code);
        return IOTX_ERR_INVALID_PARAM;
    }
    message = (CoAPMessage *)p_message;
    *p_resp_code   = (iotx_coap_resp_code_t) message->header.code;

    return IOTX_SUCCESS;
}


iotx_coap_context_t *IOT_CoAP_Init(iotx_coap_config_t *p_config)
{
    CoAPInitParam param;
    char url[128] = {0};
    iotx_coap_t *p_iotx_coap = NULL;

    if (NULL == p_config) {
        COAP_ERR("Invalid paramter p_config %p", p_config);
        return NULL;
    }
    if (NULL == p_config->p_devinfo) {
        COAP_ERR("Invalid paramter p_devinfo %p", p_config->p_devinfo);
        return NULL;
    }

    p_iotx_coap = coap_malloc(sizeof(iotx_coap_t));
    if (NULL == p_iotx_coap) {
        COAP_ERR(" Allocate memory for iotx_coap_context_t failed");
        return NULL;
    }
    memset(p_iotx_coap, 0x00, sizeof(iotx_coap_t));

    p_iotx_coap->p_auth_token = coap_malloc(IOTX_AUTH_TOKEN_LEN);
    if (NULL == p_iotx_coap->p_auth_token) {
        COAP_ERR(" Allocate memory for auth token failed");
        goto err;
    }
    memset(p_iotx_coap->p_auth_token, 0x00, IOTX_AUTH_TOKEN_LEN);

    /*Set the client isn't authed*/
    p_iotx_coap->is_authed = IOT_FALSE;
    p_iotx_coap->auth_token_len = IOTX_AUTH_TOKEN_LEN;

    /*Get deivce information*/
    p_iotx_coap->p_devinfo = coap_malloc(sizeof(iotx_deviceinfo_t));
    if (NULL == p_iotx_coap->p_devinfo) {
        COAP_ERR(" Allocate memory for iotx_deviceinfo_t failed");
        goto err;
    }
    memset(p_iotx_coap->p_devinfo, 0x00, sizeof(iotx_deviceinfo_t));

    /*It should be implement by the user*/
    if (NULL != p_config->p_devinfo) {
        memset(p_iotx_coap->p_devinfo, 0x00, sizeof(iotx_deviceinfo_t));
        strncpy(p_iotx_coap->p_devinfo->device_id,    p_config->p_devinfo->device_id,   IOTX_DEVICE_ID_LEN);
        strncpy(p_iotx_coap->p_devinfo->product_key,  p_config->p_devinfo->product_key, IOTX_PRODUCT_KEY_LEN);
        strncpy(p_iotx_coap->p_devinfo->device_secret, p_config->p_devinfo->device_secret, IOTX_DEVICE_SECRET_LEN);
        strncpy(p_iotx_coap->p_devinfo->device_name,  p_config->p_devinfo->device_name, IOTX_DEVICE_NAME_LEN);
    }

    /*Init coap token*/
    p_iotx_coap->coap_token = IOTX_COAP_INIT_TOKEN;

    /*Create coap context*/
    memset(&param, 0x00, sizeof(CoAPInitParam));

    if (NULL !=  p_config->p_url) {
        param.url = p_config->p_url;
    } else {
        HAL_Snprintf(url, sizeof(url), IOTX_COAP_ONLINE_DTLS_SERVER_URL, p_iotx_coap->p_devinfo->product_key);
        param.url = url;
        COAP_INFO("Using default CoAP server: %s", url);
    }
    param.maxcount = IOTX_LIST_MAX_ITEM;
    param.notifier = (CoAPEventNotifier)iotx_event_notifyer;
    param.waittime = p_config->wait_time_ms;
    p_iotx_coap->p_coap_ctx = CoAPContext_create(&param);
    if (NULL == p_iotx_coap->p_coap_ctx) {
        COAP_ERR(" Create coap context failed");
        goto err;
    }


    /*Register the event handle to notify the application */
    p_iotx_coap->event_handle = p_config->event_handle;

    return (iotx_coap_context_t *)p_iotx_coap;
err:
    /* Error, release the memory */
    if (NULL != p_iotx_coap) {
        if (NULL != p_iotx_coap->p_devinfo) {
            coap_free(p_iotx_coap->p_devinfo);
        }
        if (NULL != p_iotx_coap->p_auth_token) {
            coap_free(p_iotx_coap->p_auth_token);
        }
        if (NULL != p_iotx_coap->p_coap_ctx) {
            CoAPContext_free(p_iotx_coap->p_coap_ctx);
        }

        p_iotx_coap->auth_token_len = 0;
        p_iotx_coap->is_authed = IOT_FALSE;
        coap_free(p_iotx_coap);
    }
    return NULL;
}

void IOT_CoAP_Deinit(iotx_coap_context_t **pp_context)
{
    iotx_coap_t *p_iotx_coap = NULL;

    if (NULL != pp_context && NULL != *pp_context) {
        p_iotx_coap = (iotx_coap_t *)*pp_context;
        p_iotx_coap->is_authed = IOT_FALSE;
        p_iotx_coap->auth_token_len = 0;
        p_iotx_coap->coap_token = IOTX_COAP_INIT_TOKEN;

        if (NULL != p_iotx_coap->p_auth_token) {
            coap_free(p_iotx_coap->p_auth_token);
            p_iotx_coap->p_auth_token = NULL;
        }

        if (NULL != p_iotx_coap->p_devinfo) {
            coap_free(p_iotx_coap->p_devinfo);
            p_iotx_coap->p_devinfo = NULL;
        }

        if (NULL != p_iotx_coap->p_coap_ctx) {
            CoAPContext_free(p_iotx_coap->p_coap_ctx);
            p_iotx_coap->p_coap_ctx = NULL;
        }
        coap_free(p_iotx_coap);
        *pp_context = NULL;
    }
}

int IOT_CoAP_Yield(iotx_coap_context_t *p_context)
{
    iotx_coap_t *p_iotx_coap = NULL;
    p_iotx_coap = (iotx_coap_t *)p_context;
    if (NULL == p_iotx_coap || (NULL != p_iotx_coap && NULL == p_iotx_coap->p_coap_ctx)) {
        COAP_ERR("Invalid paramter");
        return IOTX_ERR_INVALID_PARAM;
    }

    return CoAPMessage_cycle(p_iotx_coap->p_coap_ctx);
}

