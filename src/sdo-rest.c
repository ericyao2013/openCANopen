#include <stdio.h>
#include <string.h>
#include <assert.h>
#include <mloop.h>

#include "canopen/sdo_req.h"
#include "canopen/eds.h"
#include "canopen.h"
#include "canopen/master.h"
#include "vector.h"
#include "rest.h"
#include "conversions.h"
#include "string-utils.h"

#define is_in_range(x, min, max) ((min) <= (x) && (x) <= (max))

struct sdo_rest_path {
	int nodeid, index, subindex;
};

struct sdo_rest_context {
	struct rest_client* client;
	const struct eds_obj* eds_obj;
	struct sdo_rest_path path;
};

struct sdo_rest_eds_context {
	unsigned int nodeid;
	struct rest_client* client;
	const struct canopen_eds* eds;
};

static int sdo_rest__convert_path(struct sdo_rest_path* dst,
				  const struct rest_client* client)
{
	dst->nodeid = strtoul(client->req.url[1], NULL, 10);
	dst->index = strtoul(client->req.url[2], NULL, 16);
	dst->subindex = strtoul(client->req.url[3], NULL, 10);

	return (is_in_range(dst->nodeid, CANOPEN_NODEID_MIN, CANOPEN_NODEID_MAX)
	     && dst->index >= 0x1000) ? 0 : -1;
}

static const struct canopen_eds* sdo_rest__find_eds(int nodeid)
{
	struct co_master_node* node = co_master_get_node(nodeid);
	assert(node);

	const struct canopen_eds* eds;

	eds = eds_db_find(node->vendor_id, node->product_code,
			  node->revision_number);
	if (eds)
		return eds;

	return eds_db_find(node->vendor_id, node->product_code, -1);
}

static struct sdo_rest_context*
sdo_rest_context_new(struct rest_client* client, const struct eds_obj* obj,
		     const struct sdo_rest_path* path)
{
	struct sdo_rest_context* self = malloc(sizeof(*self));
	if (!self)
		return NULL;

	memset(self, 0, sizeof(*self));

	self->client = client;
	self->eds_obj = obj;
	self->path = *path;

	return self;
}

static void sdo_rest_not_found(struct rest_client* client, const char* message)
{
	struct rest_reply_data reply = {
		.status_code = "404 Not Found",
		.content_type = "text/plain",
		.content_length = strlen(message),
		.content = message
	};

	rest_reply(client->output, &reply);

	client->state = REST_CLIENT_DONE;
}

static void sdo_rest_server_error(struct rest_client* client,
				  const char* message)
{
	struct rest_reply_data reply = {
		.status_code = "500 Internal Server Error",
		.content_type = "text/plain",
		.content_length = strlen(message),
		.content = message
	};

	rest_reply(client->output, &reply);

	client->state = REST_CLIENT_DONE;
}

static void sdo_rest_bad_request(struct rest_client* client,
				  const char* message)
{
	struct rest_reply_data reply = {
		.status_code = "400 Bad Request",
		.content_type = "text/plain",
		.content_length = strlen(message),
		.content = message
	};

	rest_reply(client->output, &reply);

	client->state = REST_CLIENT_DONE;
}

static void on_sdo_rest_upload_done(struct sdo_req* req)
{
	struct sdo_rest_context* context = req->context;
	assert(context);
	struct rest_client* client = context->client;
	const struct eds_obj* eds_obj = context->eds_obj;

	if (req->status != SDO_REQ_OK) {
		sdo_rest_server_error(client, sdo_strerror(req->abort_code));
		goto done;
	}

	struct canopen_data data = {
		.type = eds_obj->type,
		.data = req->data.data,
		.size = req->data.index
	};

	char buffer[256];
	char* message = canopen_data_tostring(buffer, sizeof(buffer), &data);
	if (!message) {
		sdo_rest_server_error(client, "Data conversion failed\r\n");
		goto done;
	}

	struct rest_reply_data reply = {
		.status_code = "200 OK",
		.content_type = "text/plain",
		.content_length = strlen(message),
		.content = message
	};

	rest_reply(client->output, &reply);

	client->state = REST_CLIENT_DONE;

done:
	free(context);
	sdo_req_free(req);
}

static int sdo_rest__get(struct sdo_rest_context* context)
{
	struct rest_client* client = context->client;
	struct sdo_rest_path* path = &context->path;
	const struct eds_obj* eds_obj = context->eds_obj;

	if (!(eds_obj->access & (EDS_OBJ_R | EDS_OBJ_CONST))) {
		sdo_rest_bad_request(client, "Object is not readable\r\n");
		return -1;
	}

	struct sdo_req_info info = {
		.type = SDO_REQ_UPLOAD,
		.index = path->index,
		.subindex = path->subindex,
		.on_done = on_sdo_rest_upload_done,
		.context = context
	};

	struct sdo_req* req = sdo_req_new(&info);
	if (!req) {
		sdo_rest_server_error(client, "Out of memory\r\n");
		return -1;
	}

	if (sdo_req_start(req, sdo_req_queue_get(path->nodeid)) < 0)
		goto failure;

	return 0;

failure:
	sdo_req_free(req);

	sdo_rest_server_error(client, "Failed to start sdo request\r\n");
	return -1;
}

static void on_sdo_rest_download_done(struct sdo_req* req)
{
	struct sdo_rest_context* context = req->context;
	assert(context);
	struct rest_client* client = context->client;

	if (req->status != SDO_REQ_OK) {
		sdo_rest_server_error(client, sdo_strerror(req->abort_code));
		goto done;
	}

	struct rest_reply_data reply = {
		.status_code = "200 OK",
		.content_type = "text/plain",
		.content_length = 0,
		.content = ""
	};

	rest_reply(client->output, &reply);

	client->state = REST_CLIENT_DONE;

done:
	free(context);
	sdo_req_free(req);
}

static int sdo_rest__put(struct sdo_rest_context* context, const void* content)
{
	struct rest_client* client = context->client;
	struct sdo_rest_path* path = &context->path;
	const struct eds_obj* eds_obj = context->eds_obj;
	size_t content_length = client->req.content_length;

	if (!(eds_obj->access & EDS_OBJ_W)) {
		sdo_rest_bad_request(client, "Object is not writable\r\n");
		return -1;
	}

	struct canopen_data data = { 0 };

	char* input = malloc(content_length + 1);
	if (!input) {
		sdo_rest_server_error(client, "Out of memory\r\n");
		goto failure;
	}

	memcpy(input, content, content_length);
	input[content_length] = '\0';

	int r = canopen_data_fromstring(&data, eds_obj->type,
					string_trim(input));
	free(input);

	if (r < 0) {
		sdo_rest_server_error(client, "Data conversion failed\r\n");
		return -1;
	}

	struct sdo_req_info info = {
		.type = SDO_REQ_DOWNLOAD,
		.index = path->index,
		.subindex = path->subindex,
		.on_done = on_sdo_rest_download_done,
		.context = context,
		.dl_data = data.data,
		.dl_size = data.size
	};

	struct sdo_req* req = sdo_req_new(&info);
	if (!req) {
		sdo_rest_server_error(client, "Out of memory\r\n");
		return -1;
	}

	if (sdo_req_start(req, sdo_req_queue_get(path->nodeid)) < 0)
		goto failure;

	return 0;

failure:
	sdo_req_free(req);

	sdo_rest_server_error(client, "Failed to start sdo request\r\n");
	return -1;
}

int sdo_rest__process(struct sdo_rest_context* context, const void* content)
{
	struct rest_client* client = context->client;

	switch (client->req.method) {
	case HTTP_GET: return sdo_rest__get(context);
	case HTTP_PUT: return sdo_rest__put(context, content);
	}

	abort();
	return -1;
}

ssize_t sdo_rest__read_value(FILE* out, unsigned int nodeid, int index,
			     int subindex, enum canopen_type type)
{
	struct sdo_req_info info = {
		.type = SDO_REQ_UPLOAD,
		.index = index,
		.subindex = subindex
	};

	struct sdo_req* req = sdo_req_new(&info);
	if (!req)
		goto nomem;

	if (sdo_req_start(req, sdo_req_queue_get(nodeid)) < 0)
		goto failure;

	sdo_req_wait(req);

	if (req->status != SDO_REQ_OK)
		goto failure;

	struct canopen_data data = {
		.type = type,
		.data = req->data.data,
		.size = req->data.index
	};

	char buffer[256];
	char* str = canopen_data_tostring(buffer, sizeof(buffer), &data);
	if (!str)
		goto failure;

	sdo_req_free(req);
	return fprintf(out, "\"%s\"", str);

failure:
	sdo_req_free(req);
nomem:
	return fprintf(out, "null");
}

void sdo_rest__eds_job(struct mloop_work* work)
{
	char* buffer = NULL;
	size_t size = 0;
	size_t len = 0;

	struct sdo_rest_eds_context* context = mloop_work_get_context(work);
	const struct canopen_eds* eds = context->eds;
	struct rest_client* client = context->client;
	unsigned int nodeid = context->nodeid;

	int is_const, is_readable, is_writable;
	int with_value = http_req_query(&client->req, "with_value") != NULL;

	int index, subindex;

	FILE* out = open_memstream(&buffer, &size);
	if (!out) {
		sdo_rest_server_error(client, "Out of memory\r\n");
		return;
	}

	len += fprintf(out, "{\n");
	const struct eds_obj* obj = eds_obj_first(eds);
	if (!obj)
		goto done;

	goto first_object;
	do {
		len += fprintf(out, ",\n");
first_object:
		is_const = !!(obj->access & EDS_OBJ_CONST);
		is_readable = !!(obj->access & EDS_OBJ_R);
		is_writable = !!(obj->access & EDS_OBJ_W);

		index = eds_obj_index(obj);
		subindex = eds_obj_subindex(obj);

		len += fprintf(out, " \"%#x:%#x\": {\n", index, subindex);

		len += fprintf(out, "  \"type\": %u,\n", obj->type);
		if (is_const) {
			len += fprintf(out, "  \"const\": true");
		} else {
			len += fprintf(out, "  \"read-write\": [%s, %s]",
				       is_readable ? "true" : "false",
				       is_writable ? "true" : "false");
		}

		if ((is_const || is_readable) && with_value) {
			len += fprintf(out, ",\n  \"value\": ");
			len += sdo_rest__read_value(out, nodeid, index,
						    subindex, obj->type);
		}

		len += fprintf(out, "\n }");
		obj = eds_obj_next(eds, obj);
	} while (obj);
done:
	len += fprintf(out, "\n}\n");

	fclose(out);

	struct rest_reply_data reply = {
		.status_code = "200 OK",
		.content_type = "application/json",
		.content_length = len,
		.content = buffer
	};

	rest_reply(client->output, &reply);

	client->state = REST_CLIENT_DONE;

	free(buffer);
	return;

failure:
	fclose(out);
	free(buffer);
	return;
}

int sdo_rest__send_eds(struct rest_client* client)
{
	unsigned int nodeid = strtoul(client->req.url[1], NULL, 10);
	if (!is_in_range(nodeid, CANOPEN_NODEID_MIN, CANOPEN_NODEID_MAX)) {
		sdo_rest_not_found(client, "URL is out of range\r\n");
		return -1;
	}

	const struct canopen_eds* eds = sdo_rest__find_eds(nodeid);
	if (!eds) {
		sdo_rest_server_error(client, "Could not find EDS for node\r\n");
		return -1;
	}

	struct sdo_rest_eds_context* context = malloc(sizeof(*context));
	context->client = client;
	context->eds = eds;
	context->nodeid = nodeid;

	struct mloop_work* work = mloop_work_new();
	if (!work) {
		sdo_rest_server_error(client, "Out of memory\r\n");
		goto failure;
	}

	mloop_work_set_context(work, context, free);
	mloop_work_set_work_fn(work, sdo_rest__eds_job);
	if (mloop_start_work(mloop_default(), work) < 0)
		sdo_rest_server_error(client, "Failed to schedule response\r\n");

	mloop_work_unref(work);
	return 0;

failure:
	free(context);
	return -1;
}

void sdo_rest_service(struct rest_client* client, const void* content)
{
	if (client->req.url_index == 2 && client->req.method == HTTP_GET) {
		sdo_rest__send_eds(client);
		return;
	}

	if (client->req.url_index < 4) {
		sdo_rest_not_found(client, "Wrong URL format. Must be /sdo/<nodeid>/<index>/<subindex>\r\n");
		return;
	}

	struct sdo_rest_path path;
	if (sdo_rest__convert_path(&path, client) < 0) {
		sdo_rest_not_found(client, "URL is out of range\r\n");
		return;
	}

	const struct canopen_eds* eds = sdo_rest__find_eds(path.nodeid);
	if (!eds) {
		sdo_rest_server_error(client, "Could not find EDS for node\r\n");
		return;
	}

	const struct eds_obj* eds_obj = eds_obj_find(eds, path.index,
						     path.subindex);
	if (!eds_obj) {
		sdo_rest_not_found(client, "Index/subindex not found in EDS\r\n");
		return;
	}

	struct sdo_rest_context* context;
	context = sdo_rest_context_new(client, eds_obj, &path);
	if (!context) {
		sdo_rest_server_error(client, "Out of memory\r\n");
		return;
	}

	if (sdo_rest__process(context, content) < 0)
		free(context);
}