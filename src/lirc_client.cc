#include <lirc/lirc_client.h>

#include <inttypes.h>
#include <node/uv.h>
#include <stdlib.h>
#include <string.h>
#include <string>
#include <node.h>
#include <v8.h>

using namespace std;
using namespace v8;
using namespace node;

// https://github.com/mscdex/node-ncurses/blob/master/src/binding.cc
// Was used as an example for this source

#define MAX_CONFIGS 20

static Persistent<String> emit_symbol;
static Persistent<String> data_symbol;
static Persistent<String> rawdata_symbol;
static Persistent<String> closed_symbol;
static Persistent<String> isConnected_symbol;
static Persistent<String> mode_symbol;
static Persistent<String> configFiles_symbol;
static Persistent<Function> global_cb;

static int lircd_fd = -1;
static Persistent<String> gProgramName;
static Persistent<Boolean> gVerbose;

static uv_poll_t *read_watcher_ = NULL;

struct Tlirc_config {
	struct lirc_config *lirc_config_;
};

static Tlirc_config *my_lirc_config = new Tlirc_config[MAX_CONFIGS];
static bool closed = true;
static Persistent<Array> configFiles_;


char *string2char(const Local<String> avalue) {

	v8::String::Utf8Value utf8_value(avalue);

	std::string str = std::string(*utf8_value);
	char * writable = new char[str.size() + 1];
	std::copy(str.begin(), str.end(), writable);
	writable[str.size()] = '\0'; // don't forget the terminating 0

	return writable;
}

static void io_event (uv_poll_t* req, int status, int revents);
void addConfig(Local<String> name);
void addConfig(Handle<Array> name);
void connect(Handle<String> programname, Handle<Boolean> verbose, Handle<Array> configfiles, Handle<Function> cb);

void connect(Handle<String> programname, Handle<Boolean> verbose, Handle<String> configfiles, Handle<Function> cb) {
	Isolate* isolate = Isolate::GetCurrent();
	Local<Array> tmpArray = Array::New(isolate, 1);
	tmpArray->Set(Number::New(isolate, 0), configfiles);
	connect(programname, verbose, tmpArray, cb);
}

void connect(Handle<String> programname, Handle<Boolean> verbose, Handle<Array> configfiles, Handle<Function> cb) {
	Isolate* isolate = Isolate::GetCurrent();

	if (!closed) return;

	closed = true;

	gProgramName.Reset ( isolate, programname );
	gVerbose.Reset ( isolate, verbose );

	for (int i=0; i < MAX_CONFIGS; i++) {
		my_lirc_config[i].lirc_config_ = NULL;
	}

	if (lircd_fd == -1) {
		char * writable = string2char(Local<String>::New(isolate, gProgramName));
		lircd_fd = lirc_init(writable, verbose->Value() == true ? 1 : 0);
		delete[] writable;
	}

	if (lircd_fd < 0) {
		isolate->ThrowException(Exception::Error(String::NewFromUtf8(isolate, "Error on lirc_init.")));
		return;
	}

	configFiles_.Reset ( isolate, Array::New(isolate) );
	addConfig(configfiles);

	if (read_watcher_ == NULL) {
		read_watcher_ = new uv_poll_t;
		read_watcher_->data = my_lirc_config;
		// Setup input listener
		uv_poll_init(uv_default_loop(), read_watcher_, lircd_fd);
		// Start input listener
		uv_poll_start(read_watcher_, UV_READABLE, io_event);
	}

	global_cb.Reset (isolate, cb);

	closed = false;

}

static void on_handle_close (uv_handle_t *handle) {
	delete handle;
}

void close() {

	if (closed) return;

	for (int i=0; i < MAX_CONFIGS; i++) {
		if (my_lirc_config[i].lirc_config_ != NULL) {
			lirc_freeconfig(my_lirc_config[i].lirc_config_);
		}
		my_lirc_config[i].lirc_config_ = NULL;
	}

	uv_poll_stop(read_watcher_);
	uv_close((uv_handle_t *)read_watcher_, on_handle_close);

	read_watcher_ = NULL;
	lirc_deinit();
	lircd_fd = -1;

	closed = true;
}

void addConfig(Local<String> name) {
	Isolate* isolate = Isolate::GetCurrent();

	int i = 0;
	while ((i < MAX_CONFIGS) && (my_lirc_config[i].lirc_config_ != NULL)) {
		i++;
	}

	if (i < MAX_CONFIGS) {
		char * writable = NULL;
		if (name->Length() > 0) {
			writable = string2char(Local<String>::New (isolate, name));
		}

		if (lirc_readconfig(writable, &(my_lirc_config[i].lirc_config_), NULL) != 0) {
			isolate->ThrowException(Exception::Error(String::Concat(String::NewFromUtf8(isolate, "Error on lirc_readconfig for file:"),name)));
			delete[] writable;
			return;
		}

		delete[] writable;

		Local<Array> localConfigFiles_ = Local<Array>::New(isolate, configFiles_);
		uint32_t oldLength = localConfigFiles_->Length ();

		localConfigFiles_->Set(String::NewFromUtf8(isolate, "length"), Number::New(isolate, oldLength+1));
		localConfigFiles_->Set(Number::New(isolate, oldLength), name);
	}
	else {
		isolate->ThrowException(Exception::Error(String::NewFromUtf8(isolate, "Config buffer is full.")));
	}
}

void addConfig(Handle<Array> names) {
	Isolate* isolate = Isolate::GetCurrent();

	int length = names->Length();
	for(int i = 0; i < length; i++) {
		if (!names->Get(i)->IsString()) {
			isolate->ThrowException(Exception::Error(String::Concat(String::NewFromUtf8(isolate, "Array element is not a String:"),names->Get(i)->ToString())));
			return;
		}
		Local<Value> configfile = names->Get(i);
		addConfig(configfile->ToString());
	}
}

void clearConfig() {
	Isolate* isolate = Isolate::GetCurrent();

	Local<Array> localConfigFiles_ = Local<Array>::New(isolate, configFiles_);
	localConfigFiles_->Set(String::NewFromUtf8(isolate, "length"), Number::New(isolate, 0));

	for (int i=0; i < MAX_CONFIGS; i++) {
		if (my_lirc_config[i].lirc_config_ != NULL) {
			lirc_freeconfig(my_lirc_config[i].lirc_config_);
			my_lirc_config[i].lirc_config_ = NULL;
		}
	}
}


static void Connect (const FunctionCallbackInfo<Value>& args) {
	Isolate* isolate = Isolate::GetCurrent();
	EscapableHandleScope scope(isolate);

	if (!closed) {
		args.GetReturnValue().Set(Undefined(isolate));
		return;
	}

	if (args.Length() > 4) {
		isolate->ThrowException(Exception::TypeError(String::NewFromUtf8(isolate, "Only four arguments are allowed.")));
		return;
	}

	int prognameindex = -1;
	int verboseindex = -1;
	int configindex = -1;
	int cbindex = -1;

	for(int i=0; i < args.Length(); i++) {
		if (args[i]->IsString()) {
			if ((prognameindex != -1) && (configindex != -1)) {
				isolate->ThrowException(Exception::TypeError(String::NewFromUtf8(isolate, "Only two String argument are allowed (Program name and Config files).")));
				return;
			}
			if (prognameindex == -1) {
				prognameindex = i;
			}
			else {
				configindex = i;
			}
		}
		else if (args[i]->IsBoolean()) {
			if (verboseindex != -1) {
				isolate->ThrowException(Exception::TypeError(String::NewFromUtf8(isolate, "Only one boolean argument is allowed (verbose).")));
				return;
			}
			verboseindex = i;
		}
		else if (args[i]->IsFunction()) {
			if (cbindex != -1) {
				isolate->ThrowException(Exception::TypeError(String::NewFromUtf8(isolate, "Only one Callback Function argument is allowed (cb).")));
				return;
			}
			cbindex = i;
		}
		else if (args[i]->IsArray()) {
			if (configindex != -1) {
				isolate->ThrowException(Exception::TypeError(String::NewFromUtf8(isolate, "Only one Array argument is allowed (Config files).")));
				return;
			}
			configindex = i;
		}
	}

	if (prognameindex == -1) {
		isolate->ThrowException(Exception::Error(String::NewFromUtf8(isolate, "Programname is required.")));
		return;
	}

	if (cbindex == -1) {
		isolate->ThrowException(Exception::Error(String::NewFromUtf8(isolate, "Callback function is required.")));
		return;
	}

	if ((configindex > -1) && (configindex < verboseindex)) {
		isolate->ThrowException(Exception::TypeError(String::NewFromUtf8(isolate, "Order of arguments is wrong. verbose must be before config files.")));
		return;
	}

	if ((configindex > -1) && (configindex < prognameindex)) {
		isolate->ThrowException(Exception::TypeError(String::NewFromUtf8(isolate, "Order of arguments is wrong. program name must be before config files.")));
		return;
	}

	if ((verboseindex > -1) && (verboseindex < prognameindex)) {
		isolate->ThrowException(Exception::TypeError(String::NewFromUtf8(isolate, "Order of arguments is wrong. program name must be before verbose.")));
		return;
	}

	if (configindex == -1) {
		connect( args[prognameindex]->ToString(), verboseindex > -1 ? args[verboseindex]->ToBoolean() : Boolean::New(isolate, false), String::NewFromUtf8(isolate, ""), Local<Function>::Cast(args[cbindex]));
	}
	else if (args[configindex]->IsArray()) {
		connect( args[prognameindex]->ToString(), verboseindex > -1 ? args[verboseindex]->ToBoolean() : Boolean::New(isolate, false), Local<Array>::Cast(args[configindex]), Local<Function>::Cast(args[cbindex]));
	}
	else if (args[configindex]->IsString()) {
		connect( args[prognameindex]->ToString(), verboseindex > -1 ? args[verboseindex]->ToBoolean() : Boolean::New(isolate, false), args[configindex]->ToString(), Local<Function>::Cast(args[cbindex]));
	}

	args.GetReturnValue().Set(Undefined (isolate));
	return;
}

static void ReConnect (const FunctionCallbackInfo<Value>& args) {
	Isolate* isolate = Isolate::GetCurrent();
	EscapableHandleScope scope(isolate);

	if (!closed) {
		args.GetReturnValue().Set(Undefined (isolate));
		return;
	}

	Local<Array> localConfigFiles_ = Local<Array>::New(isolate, configFiles_);
	int length = localConfigFiles_->Length();
	Local<Array> tmpArray = Array::New(isolate, length);
	for (int i = 0; i < length; i++) {
		tmpArray->Set(Number::New(isolate, i), localConfigFiles_->Get(i));
	}
	Local<String> localProgramName = Local<String>::New (isolate, gProgramName);
	Local<Boolean> localVerbose = Local<Boolean>::New (isolate, gVerbose);
	Local<Function> local_cb = Local<Function>::New (isolate, global_cb);
	connect(localProgramName, localVerbose, tmpArray, local_cb);

	args.GetReturnValue().Set(Undefined (isolate));
	return;
}

static void Close (const FunctionCallbackInfo<Value>& args) {
	Isolate* isolate = Isolate::GetCurrent();
	EscapableHandleScope scope(isolate);

	close();

	args.GetReturnValue().Set(Undefined (isolate));
	return;
}

static void AddConfig (const FunctionCallbackInfo<Value>& args) {
	Isolate* isolate = Isolate::GetCurrent();
	EscapableHandleScope scope(isolate);

	if (args.Length() != 1) {
		isolate->ThrowException(Exception::TypeError(String::NewFromUtf8(isolate, "Only one Array or String argument is allowed.")));
		return;
	}

	if (args[0]->IsArray()) {
		addConfig(Local<Array>::Cast(args[0]));
	}
	else if (args[0]->IsString()) {
		addConfig(args[0]->ToString());
	}
	else {
		isolate->ThrowException(Exception::TypeError(String::NewFromUtf8(isolate, "Only an Array or a String argument is allowed.")));
		return;
	}

	args.GetReturnValue().Set(Undefined (isolate));
	return;
}

static void ClearConfig (const FunctionCallbackInfo<Value>& args) {
	Isolate* isolate = Isolate::GetCurrent();
	EscapableHandleScope scope(isolate);

	clearConfig();

	args.GetReturnValue().Set(Undefined (isolate));
	return;
}

static void IsConnectedGetter (Local<String> property, const PropertyCallbackInfo<Value>& info) {
	assert(property == isConnected_symbol);

	Isolate* isolate = Isolate::GetCurrent();
	EscapableHandleScope scope(isolate);

	Local<Boolean> localBool = Boolean::New (isolate, !closed);

	info.GetReturnValue().Set(localBool);
	return;
}

static void ModeGetter (Local<String> property, const PropertyCallbackInfo<Value>& info) {
	assert(property == mode_symbol);

	Isolate* isolate = Isolate::GetCurrent();
	EscapableHandleScope scope(isolate);

	const char *mode_ = NULL;
	if (my_lirc_config[0].lirc_config_ != NULL) {
		lirc_getmode(my_lirc_config[0].lirc_config_);
	}

	if (mode_ == NULL) {
		info.GetReturnValue().Set(Undefined (isolate));
		return;
	}
	else {
		info.GetReturnValue().Set(String::NewFromUtf8(isolate, mode_, String::kNormalString, strlen(mode_)));
		return;
	}
}

static void ModeSetter (Local<String> property, Local<Value> value, const PropertyCallbackInfo<void>& info) {
	assert(property == mode_symbol);

	Isolate* isolate = Isolate::GetCurrent();
	EscapableHandleScope scope(isolate);

	if (!value->IsString()) {
		isolate->ThrowException(Exception::TypeError(String::NewFromUtf8(isolate, "Mode should be a string value")));
		return;
	}

	char * writable = string2char(Local<String>::Cast(value));

	if (my_lirc_config[0].lirc_config_ != NULL) {
		lirc_setmode(my_lirc_config[0].lirc_config_, writable);
	}
	else {
		isolate->ThrowException(Exception::TypeError(String::NewFromUtf8(isolate, "Cannot set mode on empty config")));
	}
	delete[] writable;
}

static void ConfigFilesGetter (Local<String> property, const PropertyCallbackInfo<Value>& info) {
	assert(property == configFiles_symbol);

	Isolate* isolate = Isolate::GetCurrent();
	EscapableHandleScope scope(isolate);

	Local<Array> localConfigFiles_ = Local<Array>::New (isolate, configFiles_);

	info.GetReturnValue().Set(localConfigFiles_);
	return;
}

static void io_event (uv_poll_t* req, int status, int revents) {
	Isolate* isolate = Isolate::GetCurrent();
	EscapableHandleScope scope(isolate);

	Local<Function> local_cb = Local<Function>::New (isolate, global_cb);

	if (status < 0)
		return;

	if (revents & UV_READABLE) {
		char *code;
		char *c;
		int ret;

		int result = lirc_nextcode(&code);
		if (result == 0) {
			if (code != NULL) {

				// Send rawdata event
				Handle<Value> emit_argv[2] = {
					Local<String>::New (isolate, rawdata_symbol),
					String::NewFromUtf8(isolate, code, String::kNormalString, strlen(code))
				};
				TryCatch try_catch;
				local_cb->Call(isolate->GetCurrentContext()->Global(), 2, emit_argv);
				if (try_catch.HasCaught())
					FatalException(try_catch);

				for (int i=0; i<MAX_CONFIGS; i++) {
					if (my_lirc_config[i].lirc_config_ != NULL) {
						while (((ret=lirc_code2char(my_lirc_config[i].lirc_config_,code,&c)) == 0) && (c != NULL)) {
							// Send data event.
							Handle<Value> emit_argv[3] = {
								Local<String>::New (isolate, data_symbol),
								String::NewFromUtf8(isolate, c, String::kNormalString, strlen(c)),
								Local<Array>::New(isolate, configFiles_)->GetInternalField(i)
							};
							TryCatch try_catch;
							local_cb->Call(isolate->GetCurrentContext()->Global(), 3, emit_argv);
							if (try_catch.HasCaught())
								FatalException(try_catch);
						}
					}
				}

				free(code);
			}
		}
		else {
			// Connection lircd got closed. Emit event.
			// Send closed event
			close();
			Handle<Value> emit_argv[1] = {
				Local<String>::New (isolate, closed_symbol),
			};
			TryCatch try_catch;
			local_cb->Call(isolate->GetCurrentContext()->Global(), 1, emit_argv);
			if (try_catch.HasCaught())
				FatalException(try_catch);
		}
	}
}

void init (Handle<Object> target) {
	Isolate* isolate = Isolate::GetCurrent();
	EscapableHandleScope scope(isolate);

	read_watcher_ = NULL;

	emit_symbol.Reset(isolate, String::NewFromUtf8(isolate, "emit"));
	rawdata_symbol.Reset(isolate, String::NewFromUtf8(isolate, "rawdata"));
	data_symbol.Reset(isolate, String::NewFromUtf8(isolate, "data"));
	closed_symbol.Reset(isolate, String::NewFromUtf8(isolate, "closed"));

	isConnected_symbol.Reset(isolate, String::NewFromUtf8 (isolate, "isConnected"));
	mode_symbol.Reset(isolate, String::NewFromUtf8 (isolate, "mode"));
	configFiles_symbol.Reset(isolate, String::NewFromUtf8 (isolate, "configFiles"));

	NODE_SET_METHOD (target, "close", Close);
	NODE_SET_METHOD (target, "connect", Connect);
	NODE_SET_METHOD (target, "reConnect", ReConnect);
	NODE_SET_METHOD (target, "addConfig", AddConfig);
	NODE_SET_METHOD (target, "clearConfig", ClearConfig);

	target->SetAccessor(Local<String>::New(isolate, isConnected_symbol), IsConnectedGetter);
	target->SetAccessor(Local<String>::New(isolate, mode_symbol), ModeGetter, ModeSetter);
	target->SetAccessor(Local<String>::New(isolate, configFiles_symbol), ConfigFilesGetter);
}

NODE_MODULE(lirc_client, init);
