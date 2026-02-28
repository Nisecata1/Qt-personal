#include <QCoreApplication>
#include <QOpenGLTexture>
#include <QSurfaceFormat>

#include "qyuvopenglwidget.h"

// 瀛樺偍椤剁偣鍧愭爣鍜岀汗鐞嗗潗鏍?
// 瀛樺湪涓€璧风紦瀛樺湪vbo
// 浣跨敤glVertexAttribPointer鎸囧畾璁块棶鏂瑰紡鍗冲彲
static const GLfloat coordinate[] = {
    // 椤剁偣鍧愭爣锛屽瓨鍌?涓獂yz鍧愭爣
    // 鍧愭爣鑼冨洿涓篬-1,1],涓績鐐逛负 0,0
    // 浜岀淮鍥惧儚z濮嬬粓涓?
    // GL_TRIANGLE_STRIP鐨勭粯鍒舵柟寮忥細
    // 浣跨敤鍓?涓潗鏍囩粯鍒朵竴涓笁瑙掑舰锛屼娇鐢ㄥ悗涓変釜鍧愭爣缁樺埗涓€涓笁瑙掑舰锛屾濂戒负涓€涓煩褰?
    // x     y     z
    -1.0f,
    -1.0f,
    0.0f,
    1.0f,
    -1.0f,
    0.0f,
    -1.0f,
    1.0f,
    0.0f,
    1.0f,
    1.0f,
    0.0f,

    // 绾圭悊鍧愭爣锛屽瓨鍌?涓獂y鍧愭爣
    // 鍧愭爣鑼冨洿涓篬0,1],宸︿笅瑙掍负 0,0
    0.0f,
    1.0f,
    1.0f,
    1.0f,
    0.0f,
    0.0f,
    1.0f,
    0.0f
};

// 椤剁偣鐫€鑹插櫒
static const QString s_vertShader = R"(
    attribute vec3 vertexIn;    // xyz椤剁偣鍧愭爣
    attribute vec2 textureIn;   // xy绾圭悊鍧愭爣
    varying vec2 textureOut;    // 浼犻€掔粰鐗囨鐫€鑹插櫒鐨勭汗鐞嗗潗鏍?
    void main(void)
    {
        gl_Position = vec4(vertexIn, 1.0);  // 1.0琛ㄧずvertexIn鏄竴涓《鐐逛綅缃?
        textureOut = textureIn; // 绾圭悊鍧愭爣鐩存帴浼犻€掔粰鐗囨鐫€鑹插櫒
    }
)";

// 鐗囨鐫€鑹插櫒
static QString s_fragShader = R"(
    varying vec2 textureOut;        // 鐢遍《鐐圭潃鑹插櫒浼犻€掕繃鏉ョ殑绾圭悊鍧愭爣
    uniform sampler2D textureY;     // uniform 绾圭悊鍗曞厓锛屽埄鐢ㄧ汗鐞嗗崟鍏冨彲浠ヤ娇鐢ㄥ涓汗鐞?
    uniform sampler2D textureU;     // sampler2D鏄?D閲囨牱鍣?
    uniform sampler2D textureV;     // 澹版槑yuv涓変釜绾圭悊鍗曞厓
    void main(void)
    {
        vec3 yuv;
        vec3 rgb;

        // SDL2 BT709_SHADER_CONSTANTS
        // https://github.com/spurious/SDL-mirror/blob/4ddd4c445aa059bb127e101b74a8c5b59257fbe2/src/render/opengl/SDL_shaders_gl.c#L102
        const vec3 Rcoeff = vec3(1.1644,  0.000,  1.7927);
        const vec3 Gcoeff = vec3(1.1644, -0.2132, -0.5329);
        const vec3 Bcoeff = vec3(1.1644,  2.1124,  0.000);

        // 鏍规嵁鎸囧畾鐨勭汗鐞唗extureY鍜屽潗鏍噒extureOut鏉ラ噰鏍?
        yuv.x = texture2D(textureY, textureOut).r;
        yuv.y = texture2D(textureU, textureOut).r - 0.5;
        yuv.z = texture2D(textureV, textureOut).r - 0.5;

        // 閲囨牱瀹岃浆涓簉gb
        // 鍑忓皯涓€浜涗寒搴?
        yuv.x = yuv.x - 0.0625;
        rgb.r = dot(yuv, Rcoeff);
        rgb.g = dot(yuv, Gcoeff);
        rgb.b = dot(yuv, Bcoeff);
        // 杈撳嚭棰滆壊鍊?
        gl_FragColor = vec4(rgb, 1.0);
    }
)";

QYUVOpenGLWidget::QYUVOpenGLWidget(QWidget *parent) : QOpenGLWidget(parent)
{
    /*
    QSurfaceFormat format = QSurfaceFormat::defaultFormat();
    format.setColorSpace(QSurfaceFormat::sRGBColorSpace);
    format.setProfile(QSurfaceFormat::CompatibilityProfile);
    format.setMajorVersion(3);
    format.setMinorVersion(2);
    QSurfaceFormat::setDefaultFormat(format);
    */
}

QYUVOpenGLWidget::~QYUVOpenGLWidget()
{
    makeCurrent();
    m_vbo.destroy();
    deInitTextures();
    doneCurrent();
}

QSize QYUVOpenGLWidget::minimumSizeHint() const
{
    return QSize(50, 50);
}

QSize QYUVOpenGLWidget::sizeHint() const
{
    return size();
}

void QYUVOpenGLWidget::setFrameSize(const QSize &frameSize)
{
    if (m_frameSize != frameSize) {
        m_frameSize = frameSize;
        m_needUpdate = true;
        // inittexture immediately
        repaint();
    }
}

const QSize &QYUVOpenGLWidget::frameSize()
{
    return m_frameSize;
}

void QYUVOpenGLWidget::updateTextures(quint8 *dataY, quint8 *dataU, quint8 *dataV, quint32 linesizeY, quint32 linesizeU, quint32 linesizeV)
{
    if (m_textureInited) {
        updateTexture(m_texture[0], 0, dataY, linesizeY);
        updateTexture(m_texture[1], 1, dataU, linesizeU);
        updateTexture(m_texture[2], 2, dataV, linesizeV);
        update();
    }
}

void QYUVOpenGLWidget::initializeGL()
{
    initializeOpenGLFunctions();
    glDisable(GL_DEPTH_TEST);

    // 椤剁偣缂撳啿瀵硅薄鍒濆鍖?
    m_vbo.create();
    m_vbo.bind();
    m_vbo.allocate(coordinate, sizeof(coordinate));
    initShader();
    // 璁剧疆鑳屾櫙娓呯悊鑹蹭负榛戣壊
    glClearColor(0.0, 0.0, 0.0, 1.0);
    // 娓呯悊棰滆壊鑳屾櫙
    glClear(GL_COLOR_BUFFER_BIT);
}

void QYUVOpenGLWidget::paintGL()
{
    glClear(GL_COLOR_BUFFER_BIT);
    m_shaderProgram.bind();

    if (m_needUpdate) {
        deInitTextures();
        initTextures();
        m_needUpdate = false;
    }

    if (m_textureInited && m_frameSize.width() > 0 && m_frameSize.height() > 0) {
        const int viewW = width();
        const int viewH = height();

        int vpX = 0;
        int vpY = 0;
        int vpW = qMax(1, viewW);
        int vpH = qMax(1, viewH);

        if (viewW > 0 && viewH > 0) {
            if (m_frameSize.width() == m_frameSize.height()) {
                const int side = qMax(1, qMin(viewW, viewH));
                vpW = side;
                vpH = side;
                vpX = (viewW - side) / 2;
                vpY = (viewH - side) / 2;
            } else {
                const double frameAspect = static_cast<double>(m_frameSize.width()) / static_cast<double>(m_frameSize.height());
                const double viewAspect = static_cast<double>(viewW) / static_cast<double>(viewH);
                if (viewAspect > frameAspect) {
                    vpH = viewH;
                    vpW = qMax(1, qRound(vpH * frameAspect));
                    vpX = (viewW - vpW) / 2;
                    vpY = 0;
                } else {
                    vpW = viewW;
                    vpH = qMax(1, qRound(vpW / frameAspect));
                    vpX = 0;
                    vpY = (viewH - vpH) / 2;
                }
            }
        }

        glViewport(vpX, vpY, vpW, vpH);

        glActiveTexture(GL_TEXTURE0);
        glBindTexture(GL_TEXTURE_2D, m_texture[0]);

        glActiveTexture(GL_TEXTURE1);
        glBindTexture(GL_TEXTURE_2D, m_texture[1]);

        glActiveTexture(GL_TEXTURE2);
        glBindTexture(GL_TEXTURE_2D, m_texture[2]);

        glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);

        glViewport(0, 0, qMax(1, viewW), qMax(1, viewH));
    }

    m_shaderProgram.release();
}
void QYUVOpenGLWidget::resizeGL(int width, int height)
{
    Q_UNUSED(width)
    Q_UNUSED(height)
    update();
}
void QYUVOpenGLWidget::initShader()
{
    // opengles鐨刦loat銆乮nt绛夎鎵嬪姩鎸囧畾绮惧害
    if (QCoreApplication::testAttribute(Qt::AA_UseOpenGLES)) {
        s_fragShader.prepend(R"(
                             precision mediump int;
                             precision mediump float;
                             )");
    }
    m_shaderProgram.addShaderFromSourceCode(QOpenGLShader::Vertex, s_vertShader);
    m_shaderProgram.addShaderFromSourceCode(QOpenGLShader::Fragment, s_fragShader);
    m_shaderProgram.link();
    m_shaderProgram.bind();

    // 鎸囧畾椤剁偣鍧愭爣鍦╲bo涓殑璁块棶鏂瑰紡
    // 鍙傛暟瑙ｉ噴锛氶《鐐瑰潗鏍囧湪shader涓殑鍙傛暟鍚嶇О锛岄《鐐瑰潗鏍囦负float锛岃捣濮嬪亸绉讳负0锛岄《鐐瑰潗鏍囩被鍨嬩负vec3锛屾骞呬负3涓猣loat
    m_shaderProgram.setAttributeBuffer("vertexIn", GL_FLOAT, 0, 3, 3 * sizeof(float));
    // 鍚敤椤剁偣灞炴€?
    m_shaderProgram.enableAttributeArray("vertexIn");

    // 鎸囧畾绾圭悊鍧愭爣鍦╲bo涓殑璁块棶鏂瑰紡
    // 鍙傛暟瑙ｉ噴锛氱汗鐞嗗潗鏍囧湪shader涓殑鍙傛暟鍚嶇О锛岀汗鐞嗗潗鏍囦负float锛岃捣濮嬪亸绉讳负12涓猣loat锛堣烦杩囧墠闈㈠瓨鍌ㄧ殑12涓《鐐瑰潗鏍囷級锛岀汗鐞嗗潗鏍囩被鍨嬩负vec2锛屾骞呬负2涓猣loat
    m_shaderProgram.setAttributeBuffer("textureIn", GL_FLOAT, 12 * sizeof(float), 2, 2 * sizeof(float));
    m_shaderProgram.enableAttributeArray("textureIn");

    // 鍏宠仈鐗囨鐫€鑹插櫒涓殑绾圭悊鍗曞厓鍜宱pengl涓殑绾圭悊鍗曞厓锛坥pengl涓€鑸彁渚?6涓汗鐞嗗崟鍏冿級
    m_shaderProgram.setUniformValue("textureY", 0);
    m_shaderProgram.setUniformValue("textureU", 1);
    m_shaderProgram.setUniformValue("textureV", 2);
}

void QYUVOpenGLWidget::initTextures()
{
    // 鍒涘缓绾圭悊
    glGenTextures(1, &m_texture[0]);
    glBindTexture(GL_TEXTURE_2D, m_texture[0]);
    // 璁剧疆绾圭悊缂╂斁鏃剁殑绛栫暐
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    // 璁剧疆st鏂瑰悜涓婄汗鐞嗚秴鍑哄潗鏍囨椂鐨勬樉绀虹瓥鐣?
    glTexParameterf(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameterf(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_LUMINANCE, m_frameSize.width(), m_frameSize.height(), 0, GL_LUMINANCE, GL_UNSIGNED_BYTE, nullptr);

    glGenTextures(1, &m_texture[1]);
    glBindTexture(GL_TEXTURE_2D, m_texture[1]);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameterf(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameterf(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_LUMINANCE, m_frameSize.width() / 2, m_frameSize.height() / 2, 0, GL_LUMINANCE, GL_UNSIGNED_BYTE, nullptr);

    glGenTextures(1, &m_texture[2]);
    glBindTexture(GL_TEXTURE_2D, m_texture[2]);
    // 璁剧疆绾圭悊缂╂斁鏃剁殑绛栫暐
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    // 璁剧疆st鏂瑰悜涓婄汗鐞嗚秴鍑哄潗鏍囨椂鐨勬樉绀虹瓥鐣?
    glTexParameterf(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameterf(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_LUMINANCE, m_frameSize.width() / 2, m_frameSize.height() / 2, 0, GL_LUMINANCE, GL_UNSIGNED_BYTE, nullptr);

    m_textureInited = true;
}

void QYUVOpenGLWidget::deInitTextures()
{
    if (QOpenGLFunctions::isInitialized(QOpenGLFunctions::d_ptr)) {
        glDeleteTextures(3, m_texture);
    }

    memset(m_texture, 0, sizeof(m_texture));
    m_textureInited = false;
}

void QYUVOpenGLWidget::updateTexture(GLuint texture, quint32 textureType, quint8 *pixels, quint32 stride)
{
    if (!pixels)
        return;

    QSize size = 0 == textureType ? m_frameSize : m_frameSize / 2;

    makeCurrent();
    glBindTexture(GL_TEXTURE_2D, texture);
    glPixelStorei(GL_UNPACK_ROW_LENGTH, static_cast<GLint>(stride));
    glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, size.width(), size.height(), GL_LUMINANCE, GL_UNSIGNED_BYTE, pixels);
    doneCurrent();
}

