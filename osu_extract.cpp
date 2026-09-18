#include <windows.h>
#include <shobjidl.h>
#include <gdiplus.h>
#include <filesystem>
#include <fstream>
#include <string>
#include <thread>
#include <vector>
#include <atomic>

#include "resource.h"

#pragma comment(lib, "gdiplus.lib")

namespace fs = std::filesystem;

// ============================================================
// VARIABLES
// ============================================================

Gdiplus::Image *g_backgroundImage = NULL;
Gdiplus::Image *g_logoImage = NULL;
Gdiplus::Image *g_browseImage = NULL;
Gdiplus::Image *g_launchImage = NULL;

ULONG_PTR g_gdiplusToken = 0;

// Instance du programme, nécessaire pour aller chercher les images
// et gifs qui sont embarqués DANS l'exe (ressources), et non plus
// dans des fichiers séparés à côté de l'exe.
HINSTANCE g_hInstance = NULL;

// Déclarée ici, définie plus bas : lit une ressource embarquée
// dans l'exe et la transforme en IStream utilisable par GDI+.
IStream *stream_from_resource(
    int resourceId,
    const wchar_t *resourceType
);

HWND hSourceEdit = NULL;
HWND hDestinationEdit = NULL;
HWND hGifWindow = NULL;

HFONT g_font = NULL;

enum GifState
{
    GIF_WAITING,
    GIF_WORKING,
    GIF_DONE
};

std::atomic<int> g_gifState(GIF_WAITING);
std::atomic<bool> g_running(false);

struct GifAnimation
{
    std::vector<Gdiplus::Bitmap*> frames;
    UINT currentFrame = 0;
};

GifAnimation g_waitingAnimation;
GifAnimation g_workingAnimation;
GifAnimation g_doneAnimation;

#define WM_GIF_STATE_CHANGED (WM_APP + 1)


// ============================================================
// UTF-8 -> WCHAR
// ============================================================

std::wstring utf8_to_wstring(const std::string &str)
{
    if (str.empty())
        return L"";

    int size = MultiByteToWideChar(
        CP_UTF8,
        0,
        str.c_str(),
        (int)str.size(),
        NULL,
        0
    );

    if (size <= 0)
        return L"";

    std::wstring result(size, L'\0');

    MultiByteToWideChar(
        CP_UTF8,
        0,
        str.c_str(),
        (int)str.size(),
        &result[0],
        size
    );

    return result;
}


// ============================================================
// LECTURE .OSU
// ============================================================

std::wstring read_osu_value(
    const fs::path &osuFile,
    const std::wstring &key
)
{
    std::ifstream file(
        osuFile,
        std::ios::binary
    );

    if (!file.is_open())
        return L"";

    std::string content(
        (std::istreambuf_iterator<char>(file)),
        std::istreambuf_iterator<char>()
    );

    if (
        content.size() >= 3 &&
        (unsigned char)content[0] == 0xEF &&
        (unsigned char)content[1] == 0xBB &&
        (unsigned char)content[2] == 0xBF
    )
    {
        content.erase(0, 3);
    }

    std::wstring unicodeContent =
        utf8_to_wstring(content);

    std::wstring searchKey =
        key + L":";

    size_t pos = 0;

    while (
        (pos = unicodeContent.find(
            searchKey,
            pos
        )) != std::wstring::npos
    )
    {
        if (
            pos == 0 ||
            unicodeContent[pos - 1] == L'\n' ||
            unicodeContent[pos - 1] == L'\r'
        )
        {
            size_t start =
                pos + searchKey.length();

            size_t end =
                unicodeContent.find(
                    L'\n',
                    start
                );

            if (end == std::wstring::npos)
                end = unicodeContent.length();

            std::wstring value =
                unicodeContent.substr(
                    start,
                    end - start
                );

            while (
                !value.empty() &&
                (
                    value.back() == L'\r' ||
                    value.back() == L'\n'
                )
            )
            {
                value.pop_back();
            }

            return value;
        }

        pos += searchKey.length();
    }

    return L"";
}


// ============================================================
// NETTOYAGE NOM
// ============================================================

std::wstring clean_filename(std::wstring name)
{
    const std::wstring invalid =
        L"<>:\"/\\|?*";

    for (wchar_t &c : name)
    {
        if (
            invalid.find(c) !=
            std::wstring::npos
        )
        {
            c = L'_';
        }
    }

    while (
        !name.empty() &&
        (
            name.back() == L' ' ||
            name.back() == L'.'
        )
    )
    {
        name.pop_back();
    }

    if (name.empty())
        name = L"Unknown";

    return name;
}


// ============================================================
// NOM UNIQUE
// ============================================================

fs::path get_unique_filename(
    const fs::path &directory,
    const std::wstring &baseName
)
{
    fs::path result =
        directory / (baseName + L".mp3");

    int counter = 1;

    while (fs::exists(result))
    {
        result =
            directory /
            (
                baseName +
                L" (" +
                std::to_wstring(counter) +
                L").mp3"
            );

        counter++;
    }

    return result;
}


// ============================================================
// CHOIX DOSSIER
// ============================================================

int browse_folder(
    HWND owner,
    const wchar_t *title,
    wchar_t *outPath,
    int outSize
)
{
    IFileOpenDialog *pFileOpen = NULL;

    HRESULT hr =
        CoCreateInstance(
            CLSID_FileOpenDialog,
            NULL,
            CLSCTX_ALL,
            IID_IFileOpenDialog,
            (void **)&pFileOpen
        );

    if (FAILED(hr))
        return 0;

    DWORD options = 0;

    pFileOpen->GetOptions(
        &options
    );

    pFileOpen->SetOptions(
        options |
        FOS_PICKFOLDERS |
        FOS_FORCEFILESYSTEM
    );

    pFileOpen->SetTitle(title);

    hr = pFileOpen->Show(owner);

    if (SUCCEEDED(hr))
    {
        IShellItem *pItem = NULL;

        hr =
            pFileOpen->GetResult(
                &pItem
            );

        if (SUCCEEDED(hr))
        {
            PWSTR path = NULL;

            hr =
                pItem->GetDisplayName(
                    SIGDN_FILESYSPATH,
                    &path
                );

            if (SUCCEEDED(hr))
            {
                wcsncpy(
                    outPath,
                    path,
                    outSize - 1
                );

                outPath[outSize - 1] = L'\0';

                CoTaskMemFree(path);
                pItem->Release();
                pFileOpen->Release();

                return 1;
            }

            pItem->Release();
        }
    }

    pFileOpen->Release();

    return 0;
}


// ============================================================
// BACKGROUND
// ============================================================

void draw_background(
    HDC hdc,
    int width,
    int height
)
{
    Gdiplus::Graphics graphics(hdc);

    graphics.Clear(
        Gdiplus::Color(
            255,
            0,
            0,
            0
        )
    );

    if (!g_backgroundImage)
        return;

    Gdiplus::ColorMatrix matrix =
    {
        1.0f, 0.0f, 0.0f, 0.0f, 0.0f,
        0.0f, 1.0f, 0.0f, 0.0f, 0.0f,
        0.0f, 0.0f, 1.0f, 0.0f, 0.0f,
        0.0f, 0.0f, 0.0f, 0.35f, 0.0f,
        0.0f, 0.0f, 0.0f, 0.0f, 1.0f
    };

    Gdiplus::ImageAttributes attributes;

    attributes.SetColorMatrix(&matrix);

    graphics.DrawImage(
        g_backgroundImage,
        Gdiplus::Rect(
            0,
            0,
            width,
            height
        ),
        0,
        0,
        g_backgroundImage->GetWidth(),
        g_backgroundImage->GetHeight(),
        Gdiplus::UnitPixel,
        &attributes
    );
}


// ============================================================
// IMAGE DES BOUTONS
// ============================================================

void draw_button_image(
    HDC hdc,
    Gdiplus::Image *image,
    RECT rect
)
{
    if (!image)
        return;

    Gdiplus::Graphics graphics(
        hdc
    );

    graphics.SetCompositingMode(
        Gdiplus::CompositingModeSourceOver
    );

    graphics.SetInterpolationMode(
        Gdiplus::InterpolationModeHighQualityBicubic
    );

    graphics.DrawImage(
        image,
        Gdiplus::Rect(
            0,
            0,
            rect.right - rect.left,
            rect.bottom - rect.top
        )
    );
}



// ============================================================
// GIF
// ============================================================

bool load_gif_frames(
    int resourceId,
    GifAnimation &animation
)
{
    IStream *pStream =
        stream_from_resource(
            resourceId,
            L"GIFFILE"
        );

    if (!pStream)
        return false;

    Gdiplus::Image image(pStream);

    pStream->Release();

    if (
        image.GetLastStatus() !=
        Gdiplus::Ok
    )
    {
        return false;
    }

    UINT dimensionCount =
        image.GetFrameDimensionsCount();

    if (dimensionCount == 0)
        return false;

    std::vector<GUID> dimensions(
        dimensionCount
    );

    image.GetFrameDimensionsList(
        dimensions.data(),
        dimensionCount
    );

    GUID dimension = dimensions[0];

    UINT frameCount =
        image.GetFrameCount(&dimension);

    if (frameCount == 0)
        return false;

    for (UINT i = 0; i < frameCount; i++)
    {
        image.SelectActiveFrame(
            &dimension,
            i
        );

        UINT width =
            image.GetWidth();

        UINT height =
            image.GetHeight();

        Gdiplus::Bitmap *frame =
            new Gdiplus::Bitmap(
                width,
                height,
                PixelFormat32bppPARGB
            );

        if (!frame)
            continue;

        Gdiplus::Graphics graphics(frame);

        graphics.SetCompositingMode(
            Gdiplus::CompositingModeSourceCopy
        );

        graphics.DrawImage(
            &image,
            0,
            0,
            width,
            height
        );

        animation.frames.push_back(frame);
    }

    animation.currentFrame = 0;

    return !animation.frames.empty();
}


GifAnimation *get_current_animation()
{
    int state = g_gifState.load();

    if (state == GIF_WORKING)
        return &g_workingAnimation;

    if (state == GIF_DONE)
        return &g_doneAnimation;

    return &g_waitingAnimation;
}


void set_gif_state(GifState state)
{
    g_gifState.store((int)state);

    if (hGifWindow)
    {
        PostMessage(
            hGifWindow,
            WM_GIF_STATE_CHANGED,
            0,
            0
        );
    }
}


// ============================================================
// FENETRE GIF
// ============================================================

LRESULT CALLBACK GifWindowProc(
    HWND hwnd,
    UINT msg,
    WPARAM wParam,
    LPARAM lParam
)
{
    switch (msg)
    {
        case WM_CREATE:
        {
            SetTimer(
                hwnd,
                1,
                50,
                NULL
            );

            return 0;
        }

        case WM_TIMER:
        {
            if (wParam == 1)
            {
                GifAnimation *animation =
                    get_current_animation();

                if (
                    animation &&
                    !animation->frames.empty()
                )
                {
                    animation->currentFrame++;

                    if (
                        animation->currentFrame >=
                        animation->frames.size()
                    )
                    {
                        animation->currentFrame = 0;
                    }

                    InvalidateRect(
                        hwnd,
                        NULL,
                        FALSE
                    );
                }
            }

            return 0;
        }

        case WM_GIF_STATE_CHANGED:
        {
            GifAnimation *animation =
                get_current_animation();

            if (animation)
                animation->currentFrame = 0;

            InvalidateRect(
                hwnd,
                NULL,
                FALSE
            );

            return 0;
        }

        case WM_ERASEBKGND:
        {
            return 1;
        }

        case WM_PAINT:
        {
            PAINTSTRUCT ps;

            HDC hdc =
                BeginPaint(
                    hwnd,
                    &ps
                );

            GifAnimation *animation =
                get_current_animation();

            if (
                animation &&
                !animation->frames.empty()
            )
            {
                Gdiplus::Bitmap *frame =
                    animation->frames[
                        animation->currentFrame
                    ];

                if (frame)
                {
                    int originalWidth =
                        (int)frame->GetWidth();

                    int originalHeight =
                        (int)frame->GetHeight();

                    // HAUTEUR FIXE : 220
                    int drawHeight = 220;

                    // LARGEUR PROPORTIONNELLE
                    int drawWidth =
                        (
                            originalWidth *
                            drawHeight
                        ) /
                        originalHeight;

                    // CENTRAGE HORIZONTAL
                    int x =
                        (700 - drawWidth) / 2;

                    Gdiplus::Graphics graphics(hdc);

                    graphics.SetCompositingMode(
                        Gdiplus::CompositingModeSourceOver
                    );

                    graphics.SetInterpolationMode(
                        Gdiplus::InterpolationModeHighQualityBicubic
                    );

                    graphics.DrawImage(
                        frame,
                        Gdiplus::Rect(
                            x,
                            0,
                            drawWidth,
                            drawHeight
                        )
                    );
                }
            }

            EndPaint(
                hwnd,
                &ps
            );

            return 0;
        }

        case WM_DESTROY:
        {
            KillTimer(
                hwnd,
                1
            );

            return 0;
        }
    }

    return DefWindowProcW(
        hwnd,
        msg,
        wParam,
        lParam
    );
}


// ============================================================
// EXTRACTION
// ============================================================

void extract_mp3(
    std::wstring source,
    std::wstring destination
)
{
    g_running.store(true);

    set_gif_state(GIF_WORKING);

    try
    {
        if (!fs::exists(source))
        {
            g_running.store(false);
            set_gif_state(GIF_DONE);
            return;
        }

        if (!fs::exists(destination))
        {
            fs::create_directories(destination);
        }

        for (
            const auto &entry :
            fs::recursive_directory_iterator(source)
        )
        {
            if (!entry.is_regular_file())
                continue;

            if (
                entry.path().extension() !=
                L".mp3"
            )
            {
                continue;
            }

            fs::path mp3File =
                entry.path();

            fs::path folder =
                mp3File.parent_path();

            fs::path osuFile;

            bool foundOsu = false;

            for (
                const auto &file :
                fs::directory_iterator(folder)
            )
            {
                if (
                    file.is_regular_file() &&
                    file.path().extension() ==
                    L".osu"
                )
                {
                    osuFile =
                        file.path();

                    foundOsu = true;

                    break;
                }
            }

            std::wstring title =
                L"Unknown Title";

            std::wstring artist =
                L"Unknown Artist";

            if (foundOsu)
            {
                title =
                    read_osu_value(
                        osuFile,
                        L"TitleUnicode"
                    );

                if (title.empty())
                {
                    title =
                        read_osu_value(
                            osuFile,
                            L"Title"
                        );
                }

                artist =
                    read_osu_value(
                        osuFile,
                        L"ArtistUnicode"
                    );

                if (artist.empty())
                {
                    artist =
                        read_osu_value(
                            osuFile,
                            L"Artist"
                        );
                }
            }

            title =
                clean_filename(title);

            artist =
                clean_filename(artist);

            std::wstring baseName =
                title +
                L" - " +
                artist;

            fs::path output =
                get_unique_filename(
                    destination,
                    baseName
                );

            try
            {
                fs::copy_file(
                    mp3File,
                    output,
                    fs::copy_options::none
                );
            }
            catch (...)
            {
            }
        }
    }
    catch (...)
    {
    }

    g_running.store(false);

    set_gif_state(GIF_DONE);
}


// ============================================================
// LOAD IMAGE / GIF DEPUIS LES RESSOURCES (intégrées dans l'exe)
// ============================================================

// Charge le contenu brut d'une ressource RCDATA (déclarée dans
// resources.rc) et retourne un IStream GDI+ prêt à être utilisé.
// C'est ce qui permet de ne PAS avoir besoin des fichiers .png/
// .jpg/.gif à côté de l'exe : tout est déjà dans le binaire.
IStream *stream_from_resource(
    int resourceId,
    const wchar_t *resourceType
)
{
    HRSRC hResInfo =
        FindResourceW(
            g_hInstance,
            MAKEINTRESOURCEW(resourceId),
            resourceType
        );

    if (!hResInfo)
        return NULL;

    DWORD size =
        SizeofResource(
            g_hInstance,
            hResInfo
        );

    if (size == 0)
        return NULL;

    HGLOBAL hResData =
        LoadResource(
            g_hInstance,
            hResInfo
        );

    if (!hResData)
        return NULL;

    void *pResData =
        LockResource(hResData);

    if (!pResData)
        return NULL;

    // On copie les données dans un bloc mémoire "global" séparé,
    // car IStream (via CreateStreamOnHGlobal) doit posséder son
    // propre buffer, distinct de celui de la ressource.
    HGLOBAL hBuffer =
        GlobalAlloc(
            GMEM_MOVEABLE,
            size
        );

    if (!hBuffer)
        return NULL;

    void *pBuffer =
        GlobalLock(hBuffer);

    if (!pBuffer)
    {
        GlobalFree(hBuffer);
        return NULL;
    }

    CopyMemory(
        pBuffer,
        pResData,
        size
    );

    GlobalUnlock(hBuffer);

    IStream *pStream = NULL;

    // TRUE => le stream prend possession de hBuffer et le
    // libèrera automatiquement à sa destruction.
    if (
        CreateStreamOnHGlobal(
            hBuffer,
            TRUE,
            &pStream
        ) != S_OK
    )
    {
        GlobalFree(hBuffer);
        return NULL;
    }

    return pStream;
}

Gdiplus::Image *load_image(
    int resourceId
)
{
    IStream *pStream =
        stream_from_resource(
            resourceId,
            L"IMAGEFILE"
        );

    if (!pStream)
        return NULL;

    Gdiplus::Image *image =
        Gdiplus::Image::FromStream(pStream);

    pStream->Release();

    if (
        !image ||
        image->GetLastStatus() != Gdiplus::Ok
    )
    {
        delete image;
        return NULL;
    }

    return image;
}


// ============================================================
// WINDOW PRINCIPALE
// ============================================================

LRESULT CALLBACK WindowProc(
    HWND hwnd,
    UINT msg,
    WPARAM wParam,
    LPARAM lParam
)
{
    switch (msg)
    {
        case WM_CREATE:
        {
            g_font =
                CreateFontW(
                    18,
                    0,
                    0,
                    0,
                    FW_NORMAL,
                    FALSE,
                    FALSE,
                    FALSE,
                    DEFAULT_CHARSET,
                    OUT_DEFAULT_PRECIS,
                    CLIP_DEFAULT_PRECIS,
                    DEFAULT_QUALITY,
                    DEFAULT_PITCH |
                    FF_DONTCARE,
                    L"Arial"
                );

            // SOURCE TITLE
            HWND hSourceTitle =
                CreateWindowW(
                    L"STATIC",
                    L"Dossier des Songs osu!",
                    WS_VISIBLE | WS_CHILD,
                    190,
                    30,
                    500,
                    25,
                    hwnd,
                    NULL,
                    NULL,
                    NULL
                );

            SendMessageW(
                hSourceTitle,
                WM_SETFONT,
                (WPARAM)g_font,
                TRUE
            );

            // SOURCE EDIT
            hSourceEdit =
                CreateWindowW(
                    L"EDIT",
                    L"",
                    WS_VISIBLE |
                    WS_CHILD |
                    ES_AUTOHSCROLL |
                    ES_LEFT,
                    190,
                    60,
                    500,
                    35,
                    hwnd,
                    NULL,
                    NULL,
                    NULL
                );

            SendMessageW(
                hSourceEdit,
                WM_SETFONT,
                (WPARAM)g_font,
                TRUE
            );

            // BOUTON SOURCE
            CreateWindowW(
                L"BUTTON",
                L"",
                WS_VISIBLE |
                WS_CHILD |
                BS_OWNERDRAW,
                700,
                57,
                150,
                42,
                hwnd,
                (HMENU)1001,
                NULL,
                NULL
            );

            // DESTINATION TITLE
            HWND hDestinationTitle =
                CreateWindowW(
                    L"STATIC",
                    L"Dossier de destination",
                    WS_VISIBLE | WS_CHILD,
                    190,
                    115,
                    500,
                    25,
                    hwnd,
                    NULL,
                    NULL,
                    NULL
                );

            SendMessageW(
                hDestinationTitle,
                WM_SETFONT,
                (WPARAM)g_font,
                TRUE
            );

            // DESTINATION EDIT
            hDestinationEdit =
                CreateWindowW(
                    L"EDIT",
                    L"",
                    WS_VISIBLE |
                    WS_CHILD |
                    ES_AUTOHSCROLL |
                    ES_LEFT,
                    190,
                    145,
                    500,
                    35,
                    hwnd,
                    NULL,
                    NULL,
                    NULL
                );

            SendMessageW(
                hDestinationEdit,
                WM_SETFONT,
                (WPARAM)g_font,
                TRUE
            );

            // BOUTON DESTINATION
            CreateWindowW(
                L"BUTTON",
                L"",
                WS_VISIBLE |
                WS_CHILD |
                BS_OWNERDRAW,
                700,
                143,
                150,
                42,
                hwnd,
                (HMENU)1002,
                NULL,
                NULL
            );

            // BOUTON LANCER
            CreateWindowW(
                L"BUTTON",
                L"",
                WS_VISIBLE |
                WS_CHILD |
                BS_OWNERDRAW,
                340,
                195,
                220,
                50,
                hwnd,
                (HMENU)1003,
                NULL,
                NULL
            );

            // GIF
            hGifWindow =
                CreateWindowW(
                    L"OsuGifWindow",
                    L"",
                    WS_VISIBLE |
                    WS_CHILD,
                    100,
                    290,
                    700,
                    220,
                    hwnd,
                    NULL,
                    NULL,
                    NULL
                );

            return 0;
        }


        // ====================================================
        // EDIT
        // ====================================================

        case WM_CTLCOLOREDIT:
        {
            HDC hdcEdit =
                (HDC)wParam;

            SetTextColor(
                hdcEdit,
                RGB(255, 255, 255)
            );

            SetBkColor(
                hdcEdit,
                RGB(0, 0, 0)
            );

            static HBRUSH blackBrush =
                CreateSolidBrush(
                    RGB(0, 0, 0)
                );

            return (LRESULT)blackBrush;
        }


        // ====================================================
        // TEXTES
        // ====================================================

        case WM_CTLCOLORSTATIC:
        {
            HDC hdcStatic =
                (HDC)wParam;

            SetTextColor(
                hdcStatic,
                RGB(255, 255, 255)
            );

            SetBkMode(
                hdcStatic,
                TRANSPARENT
            );

            return (LRESULT)GetStockObject(
                NULL_BRUSH
            );
        }


        // ====================================================
        // BOUTONS
        // ====================================================

        case WM_DRAWITEM:
        {
            DRAWITEMSTRUCT *dis =
                (DRAWITEMSTRUCT *)lParam;

            if (
                dis->CtlID == 1001 ||
                dis->CtlID == 1002 ||
                dis->CtlID == 1003
            )
            {
                Gdiplus::Graphics graphics(
                    dis->hDC
                );

                graphics.Clear(
                    Gdiplus::Color(
                        255,
                        0,
                        0,
                        0
                    )
                );

                HWND parent =
                    GetParent(
                        dis->hwndItem
                    );

                RECT clientRect;

                GetClientRect(
                    parent,
                    &clientRect
                );

                POINT position =
                {
                    0,
                    0
                };

                ClientToScreen(
                    dis->hwndItem,
                    &position
                );

                ScreenToClient(
                    parent,
                    &position
                );

                if (g_backgroundImage)
                {
                    Gdiplus::ColorMatrix matrix =
                    {
                        1.0f, 0.0f, 0.0f, 0.0f, 0.0f,
                        0.0f, 1.0f, 0.0f, 0.0f, 0.0f,
                        0.0f, 0.0f, 1.0f, 0.0f, 0.0f,
                        0.0f, 0.0f, 0.0f, 0.35f, 0.0f,
                        0.0f, 0.0f, 0.0f, 0.0f, 1.0f
                    };

                    Gdiplus::ImageAttributes attributes;

                    attributes.SetColorMatrix(
                        &matrix
                    );

                    graphics.DrawImage(
                        g_backgroundImage,
                        Gdiplus::Rect(
                            -position.x,
                            -position.y,
                            clientRect.right,
                            clientRect.bottom
                        ),
                        0,
                        0,
                        g_backgroundImage->GetWidth(),
                        g_backgroundImage->GetHeight(),
                        Gdiplus::UnitPixel,
                        &attributes
                    );
                }


                // PNG PARCOURIR
                if (
                    dis->CtlID == 1001 ||
                    dis->CtlID == 1002
                )
                {
                    draw_button_image(
                        dis->hDC,
                        g_browseImage,
                        dis->rcItem
                    );
                }


                // PNG LANCER
                if (
                    dis->CtlID == 1003
                )
                {
                    draw_button_image(
                        dis->hDC,
                        g_launchImage,
                        dis->rcItem
                    );
                }

                return TRUE;
            }

            return TRUE;
        }


        // ====================================================
        // PAINT
        // ====================================================

        case WM_PAINT:
        {
            PAINTSTRUCT ps;

            HDC hdc =
                BeginPaint(
                    hwnd,
                    &ps
                );

            RECT rect;

            GetClientRect(
                hwnd,
                &rect
            );

            // BACKGROUND
            draw_background(
                hdc,
                rect.right,
                rect.bottom
            );

            // LOGO
            if (g_logoImage)
            {
                Gdiplus::Graphics graphics(hdc);

                graphics.DrawImage(
                    g_logoImage,
                    Gdiplus::Rect(
                        20,
                        20,
                        150,
                        150
                    )
                );
            }

            // =================================================
            // CONTOURS BLANCS
            // =================================================

            HBRUSH whiteBrush =
                CreateSolidBrush(
                    RGB(
                        255,
                        255,
                        255
                    )
                );

            RECT sourceRect =
            {
                189,
                59,
                691,
                96
            };

            RECT destinationRect =
            {
                189,
                144,
                691,
                181
            };

            FrameRect(
                hdc,
                &sourceRect,
                whiteBrush
            );

            FrameRect(
                hdc,
                &destinationRect,
                whiteBrush
            );


            DeleteObject(
                whiteBrush
            );

            EndPaint(
                hwnd,
                &ps
            );

            return 0;
        }


        // ====================================================
        // COMMANDES
        // ====================================================

        case WM_COMMAND:
        {
            if (
                HIWORD(wParam) ==
                BN_CLICKED
            )
            {
                int id =
                    LOWORD(wParam);

                // SOURCE
                if (id == 1001)
                {
                    wchar_t path[MAX_PATH];

                    if (
                        browse_folder(
                            hwnd,
                            L"Choisir le dossier osu! Songs",
                            path,
                            MAX_PATH
                        )
                    )
                    {
                        SetWindowTextW(
                            hSourceEdit,
                            path
                        );
                    }

                    return 0;
                }

                // DESTINATION
                if (id == 1002)
                {
                    wchar_t path[MAX_PATH];

                    if (
                        browse_folder(
                            hwnd,
                            L"Choisir le dossier de destination",
                            path,
                            MAX_PATH
                        )
                    )
                    {
                        SetWindowTextW(
                            hDestinationEdit,
                            path
                        );
                    }

                    return 0;
                }

                // LANCER
                if (id == 1003)
                {
                    if (g_running.load())
                        return 0;

                    wchar_t source[MAX_PATH];
                    wchar_t destination[MAX_PATH];

                    GetWindowTextW(
                        hSourceEdit,
                        source,
                        MAX_PATH
                    );

                    GetWindowTextW(
                        hDestinationEdit,
                        destination,
                        MAX_PATH
                    );

                    if (wcslen(source) == 0)
                        return 0;

                    if (wcslen(destination) == 0)
                        return 0;

                    std::wstring sourcePath =
                        source;

                    std::wstring destinationPath =
                        destination;

                    std::thread(
                        extract_mp3,
                        sourcePath,
                        destinationPath
                    ).detach();

                    return 0;
                }
            }

            break;
        }


        // ====================================================
        // DESTROY
        // ====================================================

        case WM_DESTROY:
        {
            if (g_font)
            {
                DeleteObject(g_font);
                g_font = NULL;
            }

            PostQuitMessage(0);

            return 0;
        }
    }

    return DefWindowProcW(
        hwnd,
        msg,
        wParam,
        lParam
    );
}


// ============================================================
// WINMAIN
// ============================================================

int WINAPI WinMain(
    HINSTANCE hInstance,
    HINSTANCE,
    LPSTR,
    int nCmdShow
)
{
    CoInitialize(NULL);

    // ========================================================
    // GDI+
    // ========================================================

    Gdiplus::GdiplusStartupInput gdiplusStartupInput;

    if (
        Gdiplus::GdiplusStartup(
            &g_gdiplusToken,
            &gdiplusStartupInput,
            NULL
        ) != Gdiplus::Ok
    )
    {
        CoUninitialize();
        return 1;
    }

    // Nécessaire pour que FindResource / LoadResource retrouvent
    // les ressources embarquées dans CET exe.
    g_hInstance = hInstance;

    // ========================================================
    // IMAGES (toutes embarquées dans l'exe, voir resources.rc)
    // ========================================================

    g_backgroundImage =
        load_image(
            IDR_BACKGROUND
        );

    g_logoImage =
        load_image(
            IDR_LOGO
        );

    g_browseImage =
        load_image(
            IDR_BROWSE
        );

    g_launchImage =
        load_image(
            IDR_LAUNCH
        );

    // ========================================================
    // GIF (embarqués dans l'exe eux aussi)
    // ========================================================

    load_gif_frames(
        IDR_GIF_WAITING,
        g_waitingAnimation
    );

    load_gif_frames(
        IDR_GIF_WORKING,
        g_workingAnimation
    );

    load_gif_frames(
        IDR_GIF_DONE,
        g_doneAnimation
    );

    // ========================================================
    // CLASS GIF
    // ========================================================

    WNDCLASSW gifClass = {};

    gifClass.lpfnWndProc =
        GifWindowProc;

    gifClass.hInstance =
        hInstance;

    gifClass.lpszClassName =
        L"OsuGifWindow";

    gifClass.hCursor =
        LoadCursor(
            NULL,
            IDC_ARROW
        );

    gifClass.hIcon =
        LoadIconW(
            hInstance,
            MAKEINTRESOURCEW(IDI_MAINICON)
        );

    gifClass.hbrBackground =
        (HBRUSH)GetStockObject(
            BLACK_BRUSH
        );

    RegisterClassW(&gifClass);

    // ========================================================
    // CLASS PRINCIPALE
    // ========================================================

    WNDCLASSW wc = {};

    wc.lpfnWndProc =
        WindowProc;

    wc.hInstance =
        hInstance;

    wc.lpszClassName =
        L"OsuMp3Extractor";

    wc.hCursor =
        LoadCursor(
            NULL,
            IDC_ARROW
        );

    wc.hIcon =
        LoadIconW(
            hInstance,
            MAKEINTRESOURCEW(IDI_MAINICON)
        );

    wc.hbrBackground =
        (HBRUSH)GetStockObject(
            BLACK_BRUSH
        );

    RegisterClassW(&wc);

    // ========================================================
    // FENETRE
    // ========================================================

    HWND hwnd =
        CreateWindowExW(
            0,
            L"OsuMp3Extractor",
            L"osu! MP3 extract",
            WS_OVERLAPPED |
            WS_CAPTION |
            WS_SYSMENU |
            WS_MINIMIZEBOX,
            CW_USEDEFAULT,
            CW_USEDEFAULT,
            900,
            650,
            NULL,
            NULL,
            hInstance,
            NULL
        );

    if (!hwnd)
    {
        if (g_backgroundImage)
            delete g_backgroundImage;

        if (g_logoImage)
            delete g_logoImage;

        if (g_browseImage)
            delete g_browseImage;

        if (g_launchImage)
            delete g_launchImage;

        for (
            auto frame :
            g_waitingAnimation.frames
        )
            delete frame;

        for (
            auto frame :
            g_workingAnimation.frames
        )
            delete frame;

        for (
            auto frame :
            g_doneAnimation.frames
        )
            delete frame;

        Gdiplus::GdiplusShutdown(
            g_gdiplusToken
        );

        CoUninitialize();

        return 1;
    }

    // ========================================================
    // DOSSIER OSU AUTOMATIQUE
    // ========================================================

    wchar_t localAppData[MAX_PATH];

    if (
        GetEnvironmentVariableW(
            L"LOCALAPPDATA",
            localAppData,
            MAX_PATH
        )
    )
    {
        std::wstring osuSongs =
            std::wstring(localAppData) +
            L"\\osu!\\Songs";

        SetWindowTextW(
            hSourceEdit,
            osuSongs.c_str()
        );
    }

    // ========================================================
    // AFFICHAGE
    // ========================================================

    ShowWindow(
        hwnd,
        nCmdShow
    );

    UpdateWindow(hwnd);

    // ========================================================
    // MESSAGE LOOP
    // ========================================================

    MSG msg;

    while (
        GetMessageW(
            &msg,
            NULL,
            0,
            0
        ) > 0
    )
    {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }

    // ========================================================
    // CLEANUP
    // ========================================================

    if (g_backgroundImage)
        delete g_backgroundImage;

    if (g_logoImage)
        delete g_logoImage;

    if (g_browseImage)
        delete g_browseImage;

    if (g_launchImage)
        delete g_launchImage;

    for (
        auto frame :
        g_waitingAnimation.frames
    )
        delete frame;

    for (
        auto frame :
        g_workingAnimation.frames
    )
        delete frame;

    for (
        auto frame :
        g_doneAnimation.frames
    )
        delete frame;

    Gdiplus::GdiplusShutdown(
        g_gdiplusToken
    );

    CoUninitialize();

    return 0;
}