export const openApiSpec = {
  openapi: '3.0.3',
  info: {
    title: 'UniMate Backend API',
    version: '1.0.0',
    description: 'Unified Node backend for UniMate auth, feedback, and ChatBox APIs.',
  },
  servers: [
    {
      url: '/',
      description: 'Current host',
    },
    {
      url: 'http://54.206.118.226:8000',
      description: 'EC2 backend',
    },
  ],
  tags: [
    { name: 'health' },
    { name: 'auth' },
    { name: 'feedback' },
    { name: 'chat' },
    { name: 'audio' },
    { name: 'asr' },
    { name: 'tts' },
    { name: 'documents' },
    { name: 'admin' },
  ],
  components: {
    securitySchemes: {
      bearerAuth: {
        type: 'http',
        scheme: 'bearer',
        bearerFormat: 'JWT',
      },
      adminKey: {
        type: 'apiKey',
        in: 'header',
        name: 'x-admin-key',
      },
    },
    schemas: {
      Error: {
        type: 'object',
        properties: {
          message: { type: 'string' },
          detail: {},
          errors: {
            type: 'array',
            items: { type: 'string' },
          },
        },
      },
      AuthPayload: {
        type: 'object',
        required: ['email', 'password'],
        properties: {
          email: { type: 'string', example: 'admin@fpt.edu.vn' },
          password: { type: 'string', example: 'Admin@123456' },
        },
      },
      RegisterPayload: {
        type: 'object',
        required: ['name', 'email', 'password'],
        properties: {
          name: { type: 'string', example: 'Nguyen Van A' },
          email: { type: 'string', example: 'student@fpt.edu.vn' },
          password: { type: 'string', example: '123456' },
        },
      },
      FeedbackPayload: {
        type: 'object',
        required: ['name', 'email', 'phone', 'subject', 'message', 'rating'],
        properties: {
          name: { type: 'string' },
          email: { type: 'string' },
          phone: { type: 'string' },
          subject: { type: 'string' },
          message: { type: 'string' },
          rating: { type: 'integer', minimum: 1, maximum: 5 },
        },
      },
      ChatTextPayload: {
        type: 'object',
        required: ['message'],
        properties: {
          message: { type: 'string', example: 'Xin chao' },
          session_id: { type: 'string', example: 'demo' },
        },
      },
      ChatResponse: {
        type: 'object',
        properties: {
          session_id: { type: 'string' },
          input_text: { type: 'string' },
          transcript: { type: 'string' },
          reply_text: { type: 'string' },
          audio_url: { type: 'string', nullable: true },
        },
      },
      TtsPayload: {
        type: 'object',
        required: ['text'],
        properties: {
          text: { type: 'string', example: 'Xin chao sinh vien' },
          voice: { type: 'string', example: 'banmai' },
          speed: { type: 'string', example: '' },
        },
      },
    },
  },
  paths: {
    '/health': {
      get: {
        tags: ['health'],
        responses: { 200: { description: 'OK' } },
      },
    },
    '/api/v1/health': {
      get: {
        tags: ['health'],
        responses: { 200: { description: 'OK' } },
      },
    },
    '/api/auth/register': {
      post: {
        tags: ['auth'],
        requestBody: jsonBody('RegisterPayload'),
        responses: { 201: { description: 'Registration successful' }, 400: errorResponse(), 409: errorResponse() },
      },
    },
    '/api/auth/login': {
      post: {
        tags: ['auth'],
        requestBody: jsonBody('AuthPayload'),
        responses: { 200: { description: 'Login successful' }, 400: errorResponse(), 401: errorResponse() },
      },
    },
    '/api/auth/me': {
      get: {
        tags: ['auth'],
        security: [{ bearerAuth: [] }],
        responses: { 200: { description: 'User fetched successfully' }, 401: errorResponse() },
      },
    },
    '/api/feedback': {
      post: {
        tags: ['feedback'],
        security: [{ bearerAuth: [] }],
        requestBody: jsonBody('FeedbackPayload'),
        responses: { 201: { description: 'Feedback submitted' }, 401: errorResponse() },
      },
      get: {
        tags: ['feedback'],
        security: [{ bearerAuth: [] }, { adminKey: [] }],
        responses: { 200: { description: 'Feedback list' }, 401: errorResponse(), 403: errorResponse() },
      },
    },
    '/api/feedback/public': {
      get: {
        tags: ['feedback'],
        parameters: paginationParams(3, 12),
        responses: { 200: { description: 'Public feedback list' } },
      },
    },
    '/api/feedback/{id}': {
      delete: {
        tags: ['feedback'],
        security: [{ bearerAuth: [] }, { adminKey: [] }],
        parameters: [pathParam('id')],
        responses: { 200: { description: 'Feedback deleted' }, 404: errorResponse() },
      },
    },
    '/api/v1/chat/text': {
      post: {
        tags: ['chat'],
        requestBody: jsonBody('ChatTextPayload'),
        responses: { 200: schemaResponse('ChatResponse'), 400: errorResponse() },
      },
    },
    '/api/v1/chat/voice': {
      post: {
        tags: ['chat'],
        requestBody: multipartBody({
          file: { type: 'string', format: 'binary' },
          session_id: { type: 'string' },
        }),
        responses: { 200: schemaResponse('ChatResponse'), 400: errorResponse(), 422: errorResponse() },
      },
    },
    '/api/v1/chat/sessions': {
      get: {
        tags: ['chat'],
        parameters: [queryParam('limit', 'integer', 50)],
        responses: { 200: { description: 'Chat sessions' } },
      },
    },
    '/api/v1/chat/history/{sessionId}': {
      get: {
        tags: ['chat'],
        parameters: [pathParam('sessionId'), queryParam('limit', 'integer', 50)],
        responses: { 200: { description: 'Chat history' } },
      },
    },
    '/api/v1/audio/{filename}': {
      get: {
        tags: ['audio'],
        parameters: [pathParam('filename')],
        responses: { 200: { description: 'Audio file' }, 404: errorResponse() },
      },
    },
    '/api/v1/asr/fpt': {
      post: {
        tags: ['asr'],
        requestBody: multipartBody({ file: { type: 'string', format: 'binary' } }),
        responses: { 200: { description: 'FPT ASR result' }, 400: errorResponse() },
      },
    },
    '/api/v1/tts/fpt': {
      post: {
        tags: ['tts'],
        requestBody: jsonBody('TtsPayload'),
        responses: { 200: { description: 'FPT TTS result' }, 400: errorResponse() },
      },
    },
    '/api/v1/documents/upload': documentUploadPath('post'),
    '/api/v1/documents': {
      get: {
        tags: ['documents'],
        responses: { 200: { description: 'Document list' } },
      },
    },
    '/api/v1/documents/{documentId}': {
      put: {
        tags: ['documents'],
        parameters: [pathParam('documentId')],
        requestBody: multipartBody({ file: { type: 'string', format: 'binary' } }),
        responses: { 200: { description: 'Document replaced' }, 404: errorResponse() },
      },
      delete: {
        tags: ['documents'],
        parameters: [pathParam('documentId')],
        responses: { 200: { description: 'Document deleted' }, 404: errorResponse() },
      },
    },
    '/api/v1/admin/chat/sessions': {
      get: {
        tags: ['admin'],
        parameters: [queryParam('limit', 'integer', 100)],
        responses: { 200: { description: 'Admin chat sessions' } },
      },
    },
    '/api/v1/admin/chat/history/{sessionId}': {
      get: {
        tags: ['admin'],
        parameters: [pathParam('sessionId'), queryParam('limit', 'integer', 100)],
        responses: { 200: { description: 'Admin chat history' }, 404: errorResponse() },
      },
    },
    '/api/v1/admin/stats/usage': {
      get: {
        tags: ['admin'],
        parameters: [queryParam('period', 'string', 'day')],
        responses: { 200: { description: 'Usage stats' } },
      },
    },
    '/api/v1/admin/stats/top-questions': {
      get: {
        tags: ['admin'],
        parameters: [queryParam('limit', 'integer', 20)],
        responses: { 200: { description: 'Top questions' } },
      },
    },
    '/api/v1/admin/stats/unanswered': {
      get: {
        tags: ['admin'],
        parameters: [queryParam('limit', 'integer', 50)],
        responses: { 200: { description: 'Unanswered questions' } },
      },
    },
  },
};

function jsonBody(schemaName) {
  return {
    required: true,
    content: {
      'application/json': {
        schema: { $ref: `#/components/schemas/${schemaName}` },
      },
    },
  };
}

function multipartBody(properties) {
  return {
    required: true,
    content: {
      'multipart/form-data': {
        schema: {
          type: 'object',
          required: ['file'],
          properties,
        },
      },
    },
  };
}

function schemaResponse(schemaName) {
  return {
    description: 'OK',
    content: {
      'application/json': {
        schema: { $ref: `#/components/schemas/${schemaName}` },
      },
    },
  };
}

function errorResponse() {
  return {
    description: 'Error',
    content: {
      'application/json': {
        schema: { $ref: '#/components/schemas/Error' },
      },
    },
  };
}

function pathParam(name) {
  return {
    in: 'path',
    name,
    required: true,
    schema: { type: 'string' },
  };
}

function queryParam(name, type, example) {
  return {
    in: 'query',
    name,
    schema: { type, example },
  };
}

function paginationParams(defaultLimit, maxLimit) {
  return [
    queryParam('page', 'integer', 1),
    {
      ...queryParam('limit', 'integer', defaultLimit),
      description: `Max ${maxLimit}`,
    },
  ];
}

function documentUploadPath(method) {
  return {
    [method]: {
      tags: ['documents'],
      requestBody: multipartBody({ file: { type: 'string', format: 'binary' } }),
      responses: { 200: { description: 'Document uploaded' }, 400: errorResponse() },
    },
  };
}
