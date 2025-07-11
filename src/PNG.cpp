/*
Minimaliste, ce qu'il manque encore :

- Gérer tous les chunks
- Gérer tous les bits
- Gérer l'entrelacement Adam7
- CRC
- Appliquer la correction gamma et les profils de couleur
*/

#include <cstdint>
#include <optional>
#include <string>
#include <vector>
#include <windows.h>

struct RgbColor
{
    uint8_t r, g, b;
};

struct RgbaColor
{
    uint8_t r, g, b, a;
};

class PngDecoder
{
public:
    PngDecoder() = default;

    void Decode(const std::wstring& filePath);

    HBITMAP CreateBitmap(HDC deviceContext);

    uint32_t GetWidth() const { return m_width; }
    uint32_t GetHeight() const { return m_height; }

private:
    uint32_t ReadBigEndian32(std::ifstream& fileStream);
    void ParseIhdrChunk(const std::vector<uint8_t>& chunkData);
    void ParsePlteChunk(const std::vector<uint8_t>& chunkData);
    void ParseTrnsChunk(const std::vector<uint8_t>& chunkData);
    uint8_t GetBytesPerPixel() const;
    std::vector<uint8_t> DecompressZlibData(const std::vector<uint8_t>& compressedData);
    void UnfilterPixelData(const std::vector<uint8_t>& decompressedData);
    RgbaColor GetPixelRgba(uint32_t x, uint32_t y) const;

    uint32_t m_width = 0;
    uint32_t m_height = 0;
    uint8_t m_bitDepth = 0;
    uint8_t m_colorType = 0;

    std::vector<uint8_t> m_pixels;
    std::vector<RgbColor> m_palette;

    std::optional<RgbColor> m_trnsColorKey;
    std::optional<uint8_t> m_trnsGrayKey;
    std::vector<uint8_t> m_trnsPaletteAlpha;
};

#include <fstream>
#include <iostream>
#include <stdexcept>
#include <zlib.h>

void PngDecoder::Decode(const std::wstring& filePath)
{
    std::ifstream fileStream(filePath, std::ios::binary);
    if (!fileStream)
    {
        throw std::runtime_error("Impossible d'ouvrir le fichier PNG.");
    }

    const std::vector<uint8_t> kPngSignature = { 137, 80, 78, 71, 13, 10, 26, 10 };
    std::vector<uint8_t> signature(kPngSignature.size());
    fileStream.read(reinterpret_cast<char*>(signature.data()), signature.size());
    if (signature != kPngSignature)
    {
        throw std::runtime_error("Ce n'est pas un fichier PNG valide (signature incorrecte).");
    }

    std::vector<uint8_t> rawIdatData;
    bool iendFound = false;

    while (fileStream && !iendFound)
    {
        uint32_t chunkLength = ReadBigEndian32(fileStream);
        char chunkTypeStr[5] = { 0 };
        fileStream.read(chunkTypeStr, 4);

        if (fileStream.eof()) break;
        std::string chunkType = chunkTypeStr;

        std::vector<uint8_t> chunkData(chunkLength);
        if (chunkLength > 0)
        {
            fileStream.read(reinterpret_cast<char*>(chunkData.data()), chunkLength);
        }
        fileStream.seekg(4, std::ios::cur); // Ignorer le CRC

        if (chunkType == "IHDR") { ParseIhdrChunk(chunkData); }
        else if (chunkType == "PLTE") { ParsePlteChunk(chunkData); }
        else if (chunkType == "tRNS") { ParseTrnsChunk(chunkData); }
        else if (chunkType == "IDAT") { rawIdatData.insert(rawIdatData.end(), chunkData.begin(), chunkData.end()); }
        else if (chunkType == "IEND") { iendFound = true; }
    }

    if (rawIdatData.empty())
    {
        throw std::runtime_error("Aucune donnee d'image (IDAT chunk) trouvee.");
    }

    std::vector<uint8_t> decompressedData = DecompressZlibData(rawIdatData);
    UnfilterPixelData(decompressedData);

    std::cout << "Decodage termine : " << m_width << "x" << m_height << ", Type: " << (int)m_colorType << std::endl;
}

HBITMAP PngDecoder::CreateBitmap(HDC deviceContext)
{
    if (m_pixels.empty())
    {
        return NULL;
    }

    std::vector<uint8_t> dibData(static_cast<size_t>(m_width) * m_height * 4);
    const RgbColor kBackgroundColor1 = { 200, 200, 200 };
    const RgbColor kBackgroundColor2 = { 255, 255, 255 };

    for (uint32_t y = 0; y < m_height; ++y)
    {
        for (uint32_t x = 0; x < m_width; ++x)
        {
            const RgbColor& backgroundColor = ((x / 10 + y / 10) % 2 == 0) ? kBackgroundColor1 : kBackgroundColor2;
            RgbaColor foregroundColor = GetPixelRgba(x, y);

            float alphaNorm = foregroundColor.a / 255.0f;
            uint8_t finalR = static_cast<uint8_t>(foregroundColor.r * alphaNorm + backgroundColor.r * (1.0f - alphaNorm));
            uint8_t finalG = static_cast<uint8_t>(foregroundColor.g * alphaNorm + backgroundColor.g * (1.0f - alphaNorm));
            uint8_t finalB = static_cast<uint8_t>(foregroundColor.b * alphaNorm + backgroundColor.b * (1.0f - alphaNorm));

            size_t index = (static_cast<size_t>(y) * m_width + x) * 4;
            dibData[index + 0] = finalB; // Blue
            dibData[index + 1] = finalG; // Green
            dibData[index + 2] = finalR; // Red
            dibData[index + 3] = 255;    // Alpha
        }
    }

    BITMAPINFO bitmapInfo = { 0 };
    bitmapInfo.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bitmapInfo.bmiHeader.biWidth = m_width;
    bitmapInfo.bmiHeader.biHeight = -static_cast<LONG>(m_height);
    bitmapInfo.bmiHeader.biPlanes = 1;
    bitmapInfo.bmiHeader.biBitCount = 32;
    bitmapInfo.bmiHeader.biCompression = BI_RGB;

    return CreateDIBitmap(deviceContext, &bitmapInfo.bmiHeader, CBM_INIT, dibData.data(), &bitmapInfo, DIB_RGB_COLORS);
}

uint32_t PngDecoder::ReadBigEndian32(std::ifstream& fileStream)
{
    uint32_t value;
    fileStream.read(reinterpret_cast<char*>(&value), 4);
    return _byteswap_ulong(value);
}

void PngDecoder::ParseIhdrChunk(const std::vector<uint8_t>& chunkData)
{
    m_width = (chunkData[0] << 24) | (chunkData[1] << 16) | (chunkData[2] << 8) | chunkData[3];
    m_height = (chunkData[4] << 24) | (chunkData[5] << 16) | (chunkData[6] << 8) | chunkData[7];
    m_bitDepth = chunkData[8];
    m_colorType = chunkData[9];
    uint8_t interlaceMethod = chunkData[12];

    if (interlaceMethod != 0) throw std::runtime_error("L'entrelacement n'est pas gere.");
    if (m_bitDepth != 8) throw std::runtime_error("Seule une profondeur de 8 bits est geree.");
}

void PngDecoder::ParsePlteChunk(const std::vector<uint8_t>& chunkData)
{
    for (size_t i = 0; i < chunkData.size(); i += 3)
    {
        m_palette.push_back({ chunkData[i], chunkData[i + 1], chunkData[i + 2] });
    }
}

void PngDecoder::ParseTrnsChunk(const std::vector<uint8_t>& chunkData)
{
    switch (m_colorType)
    {
    case 3: m_trnsPaletteAlpha = chunkData; break;
    case 2: m_trnsColorKey = { chunkData[1], chunkData[3], chunkData[5] }; break;
    case 0: m_trnsGrayKey = chunkData[1]; break;
    }
}

uint8_t PngDecoder::GetBytesPerPixel() const
{
    switch (m_colorType)
    {
    case 0: return 1;
    case 2: return 3;
    case 3: return 1;
    case 4: return 2;
    case 6: return 4;
    default: throw std::runtime_error("Type de couleur non gere : " + std::to_string(m_colorType));
    }
}

std::vector<uint8_t> PngDecoder::DecompressZlibData(const std::vector<uint8_t>& compressedData)
{
    std::vector<uint8_t> decompressedData;
    z_stream zStream = { 0 };
    zStream.zalloc = Z_NULL;
    zStream.zfree = Z_NULL;
    zStream.opaque = Z_NULL;
    zStream.avail_in = static_cast<uInt>(compressedData.size());
    zStream.next_in = const_cast<Bytef*>(compressedData.data());

    if (inflateInit(&zStream) != Z_OK)
    {
        throw std::runtime_error("zlib inflateInit a echoue.");
    }

    constexpr size_t kChunkSize = 32768;
    std::vector<uint8_t> outChunk(kChunkSize);
    int ret;
    do
    {
        zStream.avail_out = kChunkSize;
        zStream.next_out = outChunk.data();
        ret = inflate(&zStream, Z_NO_FLUSH);
        if (ret == Z_STREAM_ERROR || ret == Z_DATA_ERROR || ret == Z_MEM_ERROR)
        {
            inflateEnd(&zStream);
            throw std::runtime_error("Erreur de decompression zlib.");
        }
        size_t have = kChunkSize - zStream.avail_out;
        decompressedData.insert(decompressedData.end(), outChunk.begin(), outChunk.begin() + have);
    } while (zStream.avail_out == 0);

    inflateEnd(&zStream);
    return decompressedData;
}

void PngDecoder::UnfilterPixelData(const std::vector<uint8_t>& decompressedData)
{
    uint8_t bpp = GetBytesPerPixel();
    size_t scanlineLength = static_cast<size_t>(m_width) * bpp;
    m_pixels.clear();
    m_pixels.reserve(static_cast<size_t>(m_height) * scanlineLength);

    std::vector<uint8_t> previousScanline(scanlineLength, 0);
    size_t dataIndex = 0;

    for (uint32_t r = 0; r < m_height; ++r)
    {
        uint8_t filterType = decompressedData[dataIndex++];
        const uint8_t* filteredScanlinePtr = &decompressedData[dataIndex];
        dataIndex += scanlineLength;

        std::vector<uint8_t> currentScanlineRecon(scanlineLength);

        for (size_t c = 0; c < scanlineLength; ++c)
        {
            uint8_t reconA = (c >= bpp) ? currentScanlineRecon[c - bpp] : 0;
            uint8_t reconB = previousScanline[c];
            uint8_t reconC = (c >= bpp) ? previousScanline[c - bpp] : 0;
            uint8_t filteredByte = filteredScanlinePtr[c];
            uint8_t reconByte = 0;

            switch (filterType)
            {
            case 0: reconByte = filteredByte; break;
            case 1: reconByte = static_cast<uint8_t>(filteredByte + reconA); break;
            case 2: reconByte = static_cast<uint8_t>(filteredByte + reconB); break;
            case 3: reconByte = static_cast<uint8_t>(filteredByte + (reconA + reconB) / 2); break;
            case 4:
            {
                int p = reconA + reconB - reconC;
                int pa = abs(p - reconA);
                int pb = abs(p - reconB);
                int pc = abs(p - reconC);
                uint8_t predictor = (pa <= pb && pa <= pc) ? reconA : (pb <= pc ? reconB : reconC);
                reconByte = static_cast<uint8_t>(filteredByte + predictor);
                break;
            }
            default: throw std::runtime_error("Type de filtre inconnu: " + std::to_string(filterType));
            }
            currentScanlineRecon[c] = reconByte;
        }
        m_pixels.insert(m_pixels.end(), currentScanlineRecon.begin(), currentScanlineRecon.end());
        previousScanline = currentScanlineRecon;
    }
}

RgbaColor PngDecoder::GetPixelRgba(uint32_t x, uint32_t y) const
{
    size_t offset = static_cast<size_t>(y) * m_width + x;
    switch (m_colorType)
    {
    case 6: return { m_pixels[offset * 4], m_pixels[offset * 4 + 1], m_pixels[offset * 4 + 2], m_pixels[offset * 4 + 3] };
    case 4: return { m_pixels[offset * 2], m_pixels[offset * 2], m_pixels[offset * 2], m_pixels[offset * 2 + 1] };
    case 2:
    {
        RgbColor c = { m_pixels[offset * 3], m_pixels[offset * 3 + 1], m_pixels[offset * 3 + 2] };
        uint8_t a = (m_trnsColorKey && m_trnsColorKey->r == c.r && m_trnsColorKey->g == c.g && m_trnsColorKey->b == c.b) ? 0 : 255;
        return { c.r, c.g, c.b, a };
    }
    case 0:
    {
        uint8_t gray = m_pixels[offset];
        uint8_t a = (m_trnsGrayKey && *m_trnsGrayKey == gray) ? 0 : 255;
        return { gray, gray, gray, a };
    }
    case 3:
    {
        uint8_t paletteIndex = m_pixels[offset];
        RgbColor c = (paletteIndex < m_palette.size()) ? m_palette[paletteIndex] : RgbColor{ 0, 0, 0 };
        uint8_t a = (paletteIndex < m_trnsPaletteAlpha.size()) ? m_trnsPaletteAlpha[paletteIndex] : 255;
        return { c.r, c.g, c.b, a };
    }
    }
    throw std::runtime_error("Combinaison de couleur/transparence non geree.");
}

PngDecoder g_pngDecoder;
HBITMAP g_hBitmap = NULL;

LRESULT CALLBACK WindowProc(HWND hwnd, UINT uMsg, WPARAM wParam, LPARAM lParam);
void CreateDebugConsole();

int WINAPI WinMain(HINSTANCE hInstance, HINSTANCE, LPSTR, int nCmdShow)
{
    CreateDebugConsole();

    int argc;
    LPWSTR* argv = CommandLineToArgvW(GetCommandLineW(), &argc);

    if (argv == NULL)
    {
        MessageBoxW(NULL, L"Erreur interne lors de la lecture de la ligne de commande.", L"Erreur Fatale", MB_OK | MB_ICONERROR);
        return 1;
    }

    if (argc < 2)
    {
        MessageBoxW(NULL, L"Aucun fichier spécifié.\n\nUsage: VotreProgramme.exe <chemin_vers_le_fichier.png>", L"Argument Manquant", MB_OK | MB_ICONINFORMATION);
        LocalFree(argv);
        return 1;
    }

    std::wstring fileName = argv[1];

    LocalFree(argv);

    try
    {
        g_pngDecoder.Decode(fileName);
    }
    catch (const std::exception& e)
    {
        std::string errorMessage = "Erreur lors du decodage du fichier '";
        errorMessage.append(std::string(fileName.begin(), fileName.end()));
        errorMessage.append("':\n\n");
        errorMessage.append(e.what());

        std::cerr << errorMessage << std::endl;
        MessageBoxA(NULL, errorMessage.c_str(), "Erreur de Decodage PNG", MB_OK | MB_ICONERROR);
        return EXIT_FAILURE;
    }

    const wchar_t kClassName[] = L"PngDecoderWindowClass";
    WNDCLASS windowClass = {};
    windowClass.lpfnWndProc = WindowProc;
    windowClass.hInstance = hInstance;
    windowClass.lpszClassName = kClassName;
    windowClass.hCursor = LoadCursor(NULL, IDC_ARROW);
    windowClass.hbrBackground = (HBRUSH)(COLOR_WINDOW + 1);
    RegisterClass(&windowClass);

    std::wstring windowTitle = L"Afficheur PNG - " + fileName;

    HWND mainWindowHandle = CreateWindowEx(
        0, kClassName, windowTitle.c_str(),
        WS_OVERLAPPEDWINDOW,
        CW_USEDEFAULT, CW_USEDEFAULT,
        g_pngDecoder.GetWidth() > 0 ? g_pngDecoder.GetWidth() : 800,
        g_pngDecoder.GetHeight() > 0 ? g_pngDecoder.GetHeight() : 600,
        NULL, NULL, hInstance, NULL
    );

    if (mainWindowHandle == NULL)
    {
        return EXIT_SUCCESS;
    }

    HDC deviceContext = GetDC(mainWindowHandle);
    g_hBitmap = g_pngDecoder.CreateBitmap(deviceContext);
    ReleaseDC(mainWindowHandle, deviceContext);

    ShowWindow(mainWindowHandle, nCmdShow);

    MSG message = {};
    while (GetMessage(&message, NULL, 0, 0))
    {
        TranslateMessage(&message);
        DispatchMessage(&message);
    }

    return EXIT_SUCCESS;
}

LRESULT CALLBACK WindowProc(HWND hwnd, UINT uMsg, WPARAM wParam, LPARAM lParam)
{
    switch (uMsg)
    {
    case WM_PAINT:
    {
        PAINTSTRUCT paintStruct;
        HDC deviceContext = BeginPaint(hwnd, &paintStruct);
        if (g_hBitmap)
        {
            HDC memoryDeviceContext = CreateCompatibleDC(deviceContext);
            HBITMAP oldBitmap = (HBITMAP)SelectObject(memoryDeviceContext, g_hBitmap);
            BITMAP bitmap;
            GetObject(g_hBitmap, sizeof(bitmap), &bitmap);
            BitBlt(deviceContext, 0, 0, bitmap.bmWidth, bitmap.bmHeight, memoryDeviceContext, 0, 0, SRCCOPY);
            SelectObject(memoryDeviceContext, oldBitmap);
            DeleteDC(memoryDeviceContext);
        }
        EndPaint(hwnd, &paintStruct);
        return EXIT_SUCCESS;
    }
    case WM_DESTROY:
    {
        if (g_hBitmap)
        {
            DeleteObject(g_hBitmap);
        }
        PostQuitMessage(0);
        return EXIT_SUCCESS;
    }
    case WM_CLOSE:
    {
        DestroyWindow(hwnd);
        return EXIT_SUCCESS;
    }
    default:
    {
        return DefWindowProc(hwnd, uMsg, wParam, lParam);
    }
    }
}

void CreateDebugConsole()
{
    if (AllocConsole())
    {
        FILE* fileStream;
        freopen_s(&fileStream, "CONOUT$", "w", stdout);
        freopen_s(&fileStream, "CONOUT$", "w", stderr);
        freopen_s(&fileStream, "CONIN$", "r", stdin);
        std::cout.clear();
        std::cerr.clear();
        std::cin.clear();
    }
}