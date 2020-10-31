/*
 * Copyright (C) 2017, David PHAM-VAN <dev.nfet.net@gmail.com>
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include "print_job.h"

#include "printing.h"

#include <fpdfview.h>
#include <objbase.h>
#include <shlobj.h>
#include <shlwapi.h>
#include <tchar.h>
#include <codecvt>
#include <fstream>
#include <iterator>
#include <numeric>

namespace nfet {

const auto pdfDpi = 72;

std::string toUtf8(std::wstring wstr) {
  int cbMultiByte =
      WideCharToMultiByte(CP_UTF8, 0, wstr.c_str(), -1, NULL, 0, NULL, NULL);
  LPSTR lpMultiByteStr = (LPSTR)malloc(cbMultiByte);
  cbMultiByte = WideCharToMultiByte(CP_UTF8, 0, wstr.c_str(), -1,
                                    lpMultiByteStr, cbMultiByte, NULL, NULL);
  std::string ret = lpMultiByteStr;
  free(lpMultiByteStr);
  return ret;
}

std::string toUtf8(TCHAR* tstr) {
#ifndef UNICODE
#error "Non unicode build not supported"
#endif

  if (!tstr) {
    return std::string{};
  }

  return toUtf8(std::wstring{tstr});
}

std::wstring fromUtf8(std::string str) {
  auto len = MultiByteToWideChar(CP_ACP, 0, str.c_str(),
                                 static_cast<int>(str.length()), nullptr, 0);
  if (len <= 0) {
    return false;
  }

  auto wstr = std::wstring{};
  wstr.resize(len);
  MultiByteToWideChar(CP_ACP, 0, str.c_str(), static_cast<int>(str.length()),
                      &wstr[0], len);

  return wstr;
}

PrintJob::PrintJob(Printing* printing, int index)
    : printing(printing), index(index) {}

bool PrintJob::directPrintPdf(std::string name,
                              std::vector<uint8_t> data,
                              std::string withPrinter) {
  FPDF_InitLibraryWithConfig(nullptr);

  auto doc = FPDF_LoadMemDocument64(data.data(), data.size(), nullptr);
  if (!doc) {
    FPDF_DestroyLibrary();
    return false;
  }

  hDC = CreateDC(TEXT("WINSPOOL"), fromUtf8(withPrinter).c_str(), NULL, NULL);
  if (!hDC) {
    FPDF_CloseDocument(doc);
    FPDF_DestroyLibrary();
    return false;
  }

  DOCINFO docInfo;
  ZeroMemory(&docInfo, sizeof(docInfo));
  docInfo.cbSize = sizeof(DOCINFO);
  auto docName = fromUtf8(name);
  docInfo.lpszDocName = docName.c_str();
  StartDoc(hDC, &docInfo);

  int pageCount = FPDF_GetPageCount(doc);

  for (auto i = 0; i < pageCount; i++) {
    StartPage(hDC);
    auto page = FPDF_LoadPage(doc, i);
    if (!page) {
      EndDoc(hDC);
      DeleteDC(hDC);
      FPDF_CloseDocument(doc);
      FPDF_DestroyLibrary();
      return false;
    }
    auto pageWidth = FPDF_GetPageWidth(page);
    auto pageHeight = FPDF_GetPageHeight(page);
    auto dpiX = GetDeviceCaps(hDC, LOGPIXELSX);
    auto dpiY = GetDeviceCaps(hDC, LOGPIXELSY);
    auto width = static_cast<int>(pageWidth / pdfDpi * dpiX);
    auto height = static_cast<int>(pageHeight / pdfDpi * dpiY);
    FPDF_RenderPage(hDC, page, 0, 0, width, height, 0, FPDF_ANNOT);
    FPDF_ClosePage(page);
    EndPage(hDC);
  }

  EndDoc(hDC);
  DeleteDC(hDC);
  FPDF_CloseDocument(doc);
  FPDF_DestroyLibrary();
  return true;
}

bool PrintJob::printPdf(std::string name) {
  PRINTDLG pd;

  // Initialize PRINTDLG
  ZeroMemory(&pd, sizeof(pd));
  pd.lStructSize = sizeof(pd);

  // Initialize PRINTDLG
  pd.hwndOwner = nullptr;
  pd.hDevMode = nullptr;   // Don't forget to free or store hDevMode.
  pd.hDevNames = nullptr;  // Don't forget to free or store hDevNames.
  pd.hDC = nullptr;
  pd.Flags = 0x0004014C;
  pd.nCopies = 1;
  pd.nFromPage = 0xFFFF;
  pd.nToPage = 0xFFFF;
  pd.nMinPage = 1;
  pd.nMaxPage = 0xFFFF;

  auto r = PrintDlg(&pd);

  if (r == 1) {
    auto dpiX = static_cast<double>(GetDeviceCaps(pd.hDC, LOGPIXELSX)) / pdfDpi;
    auto dpiY = static_cast<double>(GetDeviceCaps(pd.hDC, LOGPIXELSY)) / pdfDpi;
    auto pageWidth =
        static_cast<double>(GetDeviceCaps(pd.hDC, PHYSICALWIDTH)) / dpiX;
    auto pageHeight =
        static_cast<double>(GetDeviceCaps(pd.hDC, PHYSICALHEIGHT)) / dpiY;
    auto printableWidth =
        static_cast<double>(GetDeviceCaps(pd.hDC, HORZRES)) / dpiX;
    auto printableHeight =
        static_cast<double>(GetDeviceCaps(pd.hDC, VERTRES)) / dpiY;
    auto marginLeft =
        static_cast<double>(GetDeviceCaps(pd.hDC, PHYSICALOFFSETX)) / dpiX;
    auto marginTop =
        static_cast<double>(GetDeviceCaps(pd.hDC, PHYSICALOFFSETY)) / dpiY;
    auto marginRight = pageWidth - printableWidth - marginLeft;
    auto marginBottom = pageHeight - printableHeight - marginTop;

    hDC = pd.hDC;
    hDevMode = pd.hDevMode;
    hDevNames = pd.hDevNames;
    documentName = name;

    printing->onLayout(this, pageWidth, pageHeight, marginLeft, marginTop,
                       marginRight, marginBottom);
    return true;
  }

  return false;
}

std::vector<Printer> PrintJob::listPrinters() {
  auto printers = std::vector<Printer>{};
  DWORD needed = 0;
  DWORD returned = 0;
  const auto flags = PRINTER_ENUM_LOCAL | PRINTER_ENUM_CONNECTIONS;

  EnumPrinters(flags, nullptr, 1, nullptr, 0, &needed, &returned);

  auto buffer = (PRINTER_INFO_1*)malloc(needed);
  if (!buffer) {
    return printers;
  }

  auto result = EnumPrinters(flags, nullptr, 1, (LPBYTE)buffer, needed, &needed,
                             &returned);

  if (result == 0) {
    free(buffer);
    return printers;
  }

  for (DWORD i = 0; i < returned; i++) {
    printers.push_back(Printer{
        toUtf8(buffer[i].pName),
        toUtf8(buffer[i].pName),
        toUtf8(buffer[i].pDescription),
        toUtf8(buffer[i].pComment),
    });
  }

  free(buffer);
  return printers;
}

void PrintJob::writeJob(std::vector<uint8_t> data) {
  auto dpiX = static_cast<double>(GetDeviceCaps(hDC, LOGPIXELSX)) / pdfDpi;
  auto dpiY = static_cast<double>(GetDeviceCaps(hDC, LOGPIXELSY)) / pdfDpi;

  DOCINFO docInfo;

  ZeroMemory(&docInfo, sizeof(docInfo));
  docInfo.cbSize = sizeof(docInfo);

  auto docName = fromUtf8(documentName);
  docInfo.lpszDocName = docName.c_str();

  auto r = StartDoc(hDC, &docInfo);

  FPDF_InitLibraryWithConfig(nullptr);

  auto doc = FPDF_LoadMemDocument64(data.data(), data.size(), nullptr);
  if (!doc) {
    FPDF_DestroyLibrary();
    return;
  }

  auto pages = FPDF_GetPageCount(doc);

  for (auto pageNum = 0; pageNum < pages; pageNum++) {
    r = StartPage(hDC);

    auto page = FPDF_LoadPage(doc, pageNum);
    if (!page) {
      EndPage(hDC);
      continue;
    }

    auto pdfWidth = FPDF_GetPageWidth(page);
    auto pdfHeight = FPDF_GetPageHeight(page);

    int bWidth = static_cast<int>(pdfWidth * dpiX);
    int bHeight = static_cast<int>(pdfHeight * dpiY);

    FPDF_RenderPage(hDC, page, 0, 0, bWidth, bHeight, 0, FPDF_ANNOT);

    r = EndPage(hDC);
  }

  FPDF_CloseDocument(doc);
  FPDF_DestroyLibrary();

  r = EndDoc(hDC);

  DeleteDC(hDC);
  GlobalFree(hDevNames);
  ClosePrinter(hDevMode);
}

void PrintJob::cancelJob(std::string error) {}

bool PrintJob::sharePdf(std::vector<uint8_t> data, std::string name) {
  TCHAR lpTempPathBuffer[MAX_PATH];

  auto ret = GetTempPath(MAX_PATH, lpTempPathBuffer);
  if (ret > MAX_PATH || (ret == 0)) {
    return false;
  }

  auto filename = fromUtf8(toUtf8(lpTempPathBuffer) + "\\" + name);

  auto output_file =
      std::basic_ofstream<uint8_t>{filename, std::ios::out | std::ios::binary};
  output_file.write(data.data(), data.size());
  output_file.close();

  SHELLEXECUTEINFO ShExecInfo;
  ShExecInfo.cbSize = sizeof(SHELLEXECUTEINFO);
  ShExecInfo.fMask = NULL;
  ShExecInfo.hwnd = NULL;
  ShExecInfo.lpVerb = TEXT("open");
  ShExecInfo.lpFile = filename.c_str();
  ShExecInfo.lpParameters = NULL;
  ShExecInfo.lpDirectory = NULL;
  ShExecInfo.nShow = SW_SHOWDEFAULT;
  ShExecInfo.hInstApp = NULL;

  ret = ShellExecuteEx(&ShExecInfo);

  return ret == TRUE;
}

void PrintJob::pickPrinter(void* result) {}

void PrintJob::rasterPdf(std::vector<uint8_t> data,
                         std::vector<int> pages,
                         double scale) {
  FPDF_InitLibraryWithConfig(nullptr);

  auto doc = FPDF_LoadMemDocument64(data.data(), data.size(), nullptr);
  if (!doc) {
    FPDF_DestroyLibrary();
    printing->onPageRasterEnd(this);
    return;
  }

  auto pageCount = FPDF_GetPageCount(doc);

  if (pages.size() == 0) {
    // Use all pages
    pages.resize(pageCount);
    std::iota(std::begin(pages), std::end(pages), 0);
  }

  for (auto n : pages) {
    if (n >= pageCount) {
      continue;
    }

    auto page = FPDF_LoadPage(doc, n);
    if (!page) {
      continue;
    }

    auto width = FPDF_GetPageWidth(page);
    auto height = FPDF_GetPageHeight(page);

    auto bWidth = static_cast<int>(width * scale);
    auto bHeight = static_cast<int>(height * scale);

    auto bitmap = FPDFBitmap_Create(bWidth, bHeight, 0);
    FPDFBitmap_FillRect(bitmap, 0, 0, bWidth, bHeight, 0xffffffff);

    FPDF_RenderPageBitmap(bitmap, page, 0, 0, bWidth, bHeight, 0, FPDF_ANNOT);

    uint8_t* p = static_cast<uint8_t*>(FPDFBitmap_GetBuffer(bitmap));
    auto stride = FPDFBitmap_GetStride(bitmap);
    size_t l = static_cast<size_t>(bHeight * stride);

    // BGRA to RGBA conversion
    for (auto y = 0; y < bHeight; y++) {
      auto offset = y * stride;
      for (auto x = 0; x < bWidth; x++) {
        auto t = p[offset];
        p[offset] = p[offset + 2];
        p[offset + 2] = t;
        offset += 4;
      }
    }

    printing->onPageRasterized(std::vector<uint8_t>{p, p + l}, bWidth, bHeight,
                               this);

    FPDFBitmap_Destroy(bitmap);
  }

  FPDF_CloseDocument(doc);

  FPDF_DestroyLibrary();

  printing->onPageRasterEnd(this);
}

std::map<std::string, bool> PrintJob::printingInfo() {
  return std::map<std::string, bool>{
      {"directPrint", true},     {"dynamicLayout", true},   {"canPrint", true},
      {"canListPrinters", true}, {"canConvertHtml", false}, {"canShare", true},
      {"canRaster", true},
  };
}

}  // namespace nfet
