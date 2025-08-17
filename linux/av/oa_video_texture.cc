#include "oa_video_texture.h"

#include <epoxy/gl.h>
#include <flutter_linux/flutter_linux.h>
#include <string.h>
#include <mutex>
#include <string>


struct _OAVideoTexture {
	FlTextureGL parent_instance;

	GLuint gl_tex = 0;        // GL texture name
	int* width = nullptr;
	int* height = nullptr;

	// Optional RGBA buffer path
	GByteArray* pixels = nullptr; // RGBA8 buffer (width*height*4)

	// Optional YUV420P packed buffer: [Y][U][V]
	GByteArray* yuv = nullptr;
	gboolean has_yuv = FALSE;

	// GL resources for YUV->RGBA conversion
	GLuint y_tex = 0, u_tex = 0, v_tex = 0; // plane textures
	GLuint fbo = 0;                         // framebuffer to render into gl_tex
	GLuint program = 0;                     // YUV->RGBA shader program
	GLuint vbo = 0;                         // full-screen quad VBO
	GLint loc_aPos = -1, loc_aTex = -1;
	GLint loc_texY = -1, loc_texU = -1, loc_texV = -1;
	int64_t registered_id = 0;    // Flutter texture id once registered

	std::mutex mutex; // protects pixels/width/height

	// (no debug fields)
};

struct _OAVideoTextureClass {
	FlTextureGLClass parent_class;
};

G_DEFINE_TYPE(OAVideoTexture, oa_video_texture, fl_texture_gl_get_type())

static GLuint compile_shader(GLenum type, const char* src) {
	GLuint s = glCreateShader(type);
	glShaderSource(s, 1, &src, nullptr);
	glCompileShader(s);
	GLint ok = 0; glGetShaderiv(s, GL_COMPILE_STATUS, &ok);
	return s;
}

static GLuint create_yuv_program(GLint& loc_aPos, GLint& loc_aTex,
								 GLint& loc_texY, GLint& loc_texU, GLint& loc_texV) {
	// GLSL 120 for desktop OpenGL compatibility
	static const char* vsrc =
		"#version 120\n"
		"attribute vec2 aPos;\n"
		"attribute vec2 aTex;\n"
		"varying vec2 vTex;\n"
		"void main(){ gl_Position=vec4(aPos,0.0,1.0); vTex=aTex; }\n";
	static const char* fsrc =
		"#version 120\n"
		"varying vec2 vTex;\n"
		"uniform sampler2D texY;\n"
		"uniform sampler2D texU;\n"
		"uniform sampler2D texV;\n"
		"void main(){\n"
		"  float y = texture2D(texY, vTex).r;\n"
		"  float u = texture2D(texU, vTex).r - 0.5;\n"
		"  float v = texture2D(texV, vTex).r - 0.5;\n"
		"  float r = y + 1.402 * v;\n"
		"  float g = y - 0.344136 * u - 0.714136 * v;\n"
		"  float b = y + 1.772 * u;\n"
		"  gl_FragColor = vec4(r, g, b, 1.0);\n"
		"}\n";

	GLuint vs = compile_shader(GL_VERTEX_SHADER, vsrc);
	GLuint fs = compile_shader(GL_FRAGMENT_SHADER, fsrc);
	GLuint prog = glCreateProgram();
	glAttachShader(prog, vs);
	glAttachShader(prog, fs);
	glLinkProgram(prog);
	glDeleteShader(vs);
	glDeleteShader(fs);
	loc_aPos  = glGetAttribLocation(prog, "aPos");
	loc_aTex  = glGetAttribLocation(prog, "aTex");
	loc_texY  = glGetUniformLocation(prog, "texY");
	loc_texU  = glGetUniformLocation(prog, "texU");
	loc_texV  = glGetUniformLocation(prog, "texV");
	return prog;
}

static gboolean oa_video_texture_populate(FlTextureGL* texture,
							uint32_t* target,
							uint32_t* name,
							uint32_t* width,
							uint32_t* height,
							GError** error) {
	OAVideoTexture* self = (OAVideoTexture*)texture;

	// (no debug print)

	if (self->gl_tex == 0) {
		glGenTextures(1, &self->gl_tex);
		glBindTexture(GL_TEXTURE_2D, self->gl_tex);
		glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
		glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
		glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
		glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
	} else {
		glBindTexture(GL_TEXTURE_2D, self->gl_tex);
	}

	// Safe for odd widths/strides
	glPixelStorei(GL_UNPACK_ALIGNMENT, 1);

	// Guard reads with mutex to avoid races with setters
	std::unique_lock<std::mutex> lk(self->mutex);
	const int cur_w = (self->width != nullptr) ? *self->width : 0;
	const int cur_h = (self->height != nullptr) ? *self->height : 0;
	const int expected = cur_w * cur_h * 4;
	const gboolean have_rgba = (self->pixels != nullptr && cur_w > 0 && cur_h > 0 && (int)self->pixels->len >= expected);
	const gboolean have_yuv  = (self->has_yuv && self->yuv != nullptr && cur_w > 0 && cur_h > 0);

	if (have_yuv) {
		// Lazy-init GL resources
		if (self->program == 0) {
			self->program = create_yuv_program(self->loc_aPos, self->loc_aTex, self->loc_texY, self->loc_texU, self->loc_texV);
		}
		if (self->vbo == 0) {
			const GLfloat quad[] = {
				-1.f,-1.f,  0.f,0.f,
				 1.f,-1.f,  1.f,0.f,
				-1.f, 1.f,  0.f,1.f,
				 1.f, 1.f,  1.f,1.f,
			};
			glGenBuffers(1, &self->vbo);
			glBindBuffer(GL_ARRAY_BUFFER, self->vbo);
			glBufferData(GL_ARRAY_BUFFER, sizeof(quad), quad, GL_STATIC_DRAW);
		}
		if (self->y_tex == 0) glGenTextures(1, &self->y_tex);
		if (self->u_tex == 0) glGenTextures(1, &self->u_tex);
		if (self->v_tex == 0) glGenTextures(1, &self->v_tex);
		if (self->fbo   == 0) glGenFramebuffers(1, &self->fbo);

		// Allocate destination RGBA texture storage
		glBindTexture(GL_TEXTURE_2D, self->gl_tex);
		glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, cur_w, cur_h, 0, GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
		glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
		glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
		glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
		glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);

		const int y_size = cur_w * cur_h;
		const int uv_w = (cur_w + 1) / 2;
		const int uv_h = (cur_h + 1) / 2;
		const int uv_size = uv_w * uv_h;
		const guint8* base = self->yuv->data;
		const guint8* y_ptr = base;
		const guint8* u_ptr = base + y_size;
		const guint8* v_ptr = base + y_size + uv_size;

		glPixelStorei(GL_UNPACK_ALIGNMENT, 1);

		// Upload planes as single-channel textures (GL_LUMINANCE for broad compat)
	glBindTexture(GL_TEXTURE_2D, self->y_tex);
	glTexImage2D(GL_TEXTURE_2D, 0, GL_R8, cur_w, cur_h, 0, GL_RED, GL_UNSIGNED_BYTE, y_ptr);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
		glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
		glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);

	glBindTexture(GL_TEXTURE_2D, self->u_tex);
	glTexImage2D(GL_TEXTURE_2D, 0, GL_R8, uv_w, uv_h, 0, GL_RED, GL_UNSIGNED_BYTE, u_ptr);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
		glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
		glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);

	glBindTexture(GL_TEXTURE_2D, self->v_tex);
	glTexImage2D(GL_TEXTURE_2D, 0, GL_R8, uv_w, uv_h, 0, GL_RED, GL_UNSIGNED_BYTE, v_ptr);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
		glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
		glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);

		// Render YUV->RGBA into self->gl_tex
		glBindFramebuffer(GL_FRAMEBUFFER, self->fbo);
		glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, self->gl_tex, 0);
		glViewport(0, 0, cur_w, cur_h);
		glUseProgram(self->program);

		glActiveTexture(GL_TEXTURE0);
		glBindTexture(GL_TEXTURE_2D, self->y_tex);
		glUniform1i(self->loc_texY, 0);
		glActiveTexture(GL_TEXTURE1);
		glBindTexture(GL_TEXTURE_2D, self->u_tex);
		glUniform1i(self->loc_texU, 1);
	glActiveTexture(GL_TEXTURE2);
	glBindTexture(GL_TEXTURE_2D, self->v_tex);
	glUniform1i(self->loc_texV, 2);

		glBindBuffer(GL_ARRAY_BUFFER, self->vbo);
		glEnableVertexAttribArray(self->loc_aPos);
		glVertexAttribPointer(self->loc_aPos, 2, GL_FLOAT, GL_FALSE, 4 * sizeof(GLfloat), (const void*)0);
		glEnableVertexAttribArray(self->loc_aTex);
		glVertexAttribPointer(self->loc_aTex, 2, GL_FLOAT, GL_FALSE, 4 * sizeof(GLfloat), (const void*)(2 * sizeof(GLfloat)));

		glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);

		// (no debug dumping)

		// Cleanup state
		glDisableVertexAttribArray(self->loc_aPos);
		glDisableVertexAttribArray(self->loc_aTex);
		glBindBuffer(GL_ARRAY_BUFFER, 0);
		glUseProgram(0);
		glBindFramebuffer(GL_FRAMEBUFFER, 0);

		*width = (uint32_t)cur_w;
		*height = (uint32_t)cur_h;
		lk.unlock();
	} else if (have_rgba) {
		glTexImage2D(GL_TEXTURE_2D,
					 0,
					 GL_RGBA8,
					 cur_w,
					 cur_h,
					 0,
					 GL_RGBA,
					 GL_UNSIGNED_BYTE,
					 self->pixels->data);

		// (no debug dumping)
		*width = (uint32_t)cur_w;
		*height = (uint32_t)cur_h;
		lk.unlock();
	} else {
		lk.unlock();
		// Fallback: ensure at least 1px renders
		static const unsigned char one_px[4] = { 0xFF, 0x00, 0x00, 0xFF }; // Red
		glTexImage2D(GL_TEXTURE_2D,
					 0,
					 GL_RGBA8,
					 1,
					 1,
					 0,
					 GL_RGBA,
					 GL_UNSIGNED_BYTE,
					 one_px);
		*width = 1;
		*height = 1;
	}

	*target = GL_TEXTURE_2D;
	*name = self->gl_tex;
	return TRUE;
}

static void oa_video_texture_dispose(GObject* obj) {
	OAVideoTexture* self = OA_VIDEO_TEXTURE(obj);
	// Important: Only call OpenGL when Flutter has made a GL context current
	// (inside populate()). To respect that constraint, do not delete GL objects
	// here since dispose() may run without a current context. The GL texture will
	// be cleaned up when the context is torn down. If you need explicit cleanup,
	// add a flag and perform glDeleteTextures in the next populate() call.
	self->gl_tex = 0;
	if (self->pixels) {
		g_byte_array_unref(self->pixels);
		self->pixels = nullptr;
	}
	if (self->width) { g_free(self->width); self->width = nullptr; }
	if (self->height) { g_free(self->height); self->height = nullptr; }
	G_OBJECT_CLASS(oa_video_texture_parent_class)->dispose(obj);
}

static void oa_video_texture_class_init(OAVideoTextureClass* klass) {
	FL_TEXTURE_GL_CLASS(klass)->populate = oa_video_texture_populate;
	G_OBJECT_CLASS(klass)->dispose = oa_video_texture_dispose;
}

static void oa_video_texture_init(OAVideoTexture* self) {
	self->pixels = g_byte_array_new();
	self->yuv = g_byte_array_new();
}

OAVideoTexture* oa_video_texture_new(int width, int height) {
	OAVideoTexture* self = OA_VIDEO_TEXTURE(g_object_new(OA_VIDEO_TEXTURE_TYPE, nullptr));
	self->width = g_new(int, 1);
	self->height = g_new(int, 1);
	*self->width = width;
	*self->height = height;
	const int cap = (width > 0 && height > 0) ? width * height * 4 : 0;
	if (cap > 0) g_byte_array_set_size(self->pixels, cap);
	return self;
}

int64_t oa_video_texture_register(OAVideoTexture* self, FlTextureRegistrar* registrar) {
	if (self->registered_id != 0) return self->registered_id;
	FlTexture* base = FL_TEXTURE(self);
	gboolean ok = fl_texture_registrar_register_texture(registrar, base);
	if (!ok) { self->registered_id = 0; return 0; }
	self->registered_id = fl_texture_get_id(base);
	return self->registered_id;
}

void oa_video_texture_set_frame(OAVideoTexture* self,
							const guint8* rgba_bytes,
							gsize length,
							int width,
							int height) {
	std::lock_guard<std::mutex> lk(self->mutex);
	if (!self->width) self->width = g_new(int, 1);
	if (!self->height) self->height = g_new(int, 1);
	*self->width = width;
	*self->height = height;
	const gsize needed = (gsize)width * (gsize)height * 4u;
	if (self->pixels->len != needed) {
		g_byte_array_set_size(self->pixels, needed);
	}
	if (rgba_bytes && length >= needed) {
		memcpy(self->pixels->data, rgba_bytes, needed);
	}
	self->has_yuv = FALSE;
}

void oa_video_texture_set_yuv420p_frame(OAVideoTexture* self,
										const guint8* yuv_bytes,
										gsize length,
										int width,
										int height) {
	std::lock_guard<std::mutex> lk(self->mutex);
	if (!self->width) self->width = g_new(int, 1);
	if (!self->height) self->height = g_new(int, 1);
	*self->width = width;
	*self->height = height;
	const gsize y_size = (gsize)width * (gsize)height;
	const gsize uv_w = (width + 1) / 2;
	const gsize uv_h = (height + 1) / 2;
	const gsize uv_size = uv_w * uv_h;
	const gsize needed = y_size + uv_size * 2;
	if (self->yuv->len != needed) {
		g_byte_array_set_size(self->yuv, needed);
	}
	if (yuv_bytes && length >= needed) {
		memcpy(self->yuv->data, yuv_bytes, needed);
		self->has_yuv = TRUE;
	} else {
		self->has_yuv = FALSE;
	}
}

void oa_video_texture_mark_frame_available(OAVideoTexture* self,
								 FlTextureRegistrar* registrar) {
	fl_texture_registrar_mark_texture_frame_available(registrar, FL_TEXTURE(self));
}

