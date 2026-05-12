#include "gtest/gtest.h"

#include "TCanvas.h"
#include "TImage.h"
#include "TString.h"
#include "TSystem.h"

#include <fstream>
#include <string>
#include <vector>
#include <zlib.h>

namespace {

// Read a binary file fully into a string.
std::string SlurpFile(const TString &path)
{
   std::ifstream in(path.Data(), std::ios::binary);
   std::string data((std::istreambuf_iterator<char>(in)),
                    std::istreambuf_iterator<char>());
   return data;
}

// Try to FlateDecode every "stream ... endstream" block in `pdf` and
// concatenate the results. Blocks that fail to decode are skipped (they may
// not be Flate-compressed, e.g. ASCIIHex inline image bodies inside a
// content stream).
std::string DecodeAllFlateStreams(const std::string &pdf)
{
   std::string out;
   const std::string streamTok = "stream";
   const std::string endTok = "endstream";
   std::size_t pos = 0;
   while (true) {
      std::size_t s = pdf.find(streamTok, pos);
      if (s == std::string::npos) break;
      // Skip the "stream" keyword and the single EOL that follows it.
      std::size_t dataBegin = s + streamTok.size();
      if (dataBegin < pdf.size() && pdf[dataBegin] == '\r') ++dataBegin;
      if (dataBegin < pdf.size() && pdf[dataBegin] == '\n') ++dataBegin;
      std::size_t e = pdf.find(endTok, dataBegin);
      if (e == std::string::npos) break;
      // Strip the trailing EOL before "endstream".
      std::size_t dataEnd = e;
      if (dataEnd > dataBegin && pdf[dataEnd - 1] == '\n') --dataEnd;
      if (dataEnd > dataBegin && pdf[dataEnd - 1] == '\r') --dataEnd;

      // Inflate.
      z_stream zs{};
      zs.next_in  = reinterpret_cast<Bytef *>(const_cast<char *>(pdf.data() + dataBegin));
      zs.avail_in = static_cast<uInt>(dataEnd - dataBegin);
      if (inflateInit(&zs) == Z_OK) {
         std::vector<char> buf(64 * 1024);
         int rc = Z_OK;
         do {
            zs.next_out  = reinterpret_cast<Bytef *>(buf.data());
            zs.avail_out = static_cast<uInt>(buf.size());
            rc = inflate(&zs, Z_NO_FLUSH);
            if (rc == Z_OK || rc == Z_STREAM_END) {
               out.append(buf.data(), buf.size() - zs.avail_out);
            } else {
               break;
            }
         } while (rc != Z_STREAM_END);
         inflateEnd(&zs);
      }
      pos = e + endTok.size();
   }
   return out;
}

// Word-boundary search for a PDF operator: must be preceded and followed by
// non-alphanumeric characters so we don't accidentally match inside hex data.
bool ContainsOperator(const std::string &s, const std::string &op)
{
   std::size_t pos = 0;
   while ((pos = s.find(op, pos)) != std::string::npos) {
      bool leftOk  = (pos == 0) || !std::isalnum(static_cast<unsigned char>(s[pos - 1]));
      std::size_t right = pos + op.size();
      bool rightOk = (right >= s.size()) || !std::isalnum(static_cast<unsigned char>(s[right]));
      if (leftOk && rightOk) return true;
      ++pos;
   }
   return false;
}

} // namespace

// Render a small synthetic TImage to a PDF and verify the page's content
// stream contains a PDF inline image (BI ... EI), with /CS /RGB declared.
// This guards the TASImage::Paint → TPDF::CellArray* path: before the fix
// it emitted only a "PDF not implemented yet" warning and no image data.
TEST(TASImage, PDFEmbedsInlineImage)
{
   const TString pdfFile = "tasimage_pdf_embed.pdf";

   TImage *img = TImage::Create();
   ASSERT_NE(img, nullptr) << "TImage::Create failed (ASImage plugin missing?)";
   // Build a small two-colour image so the inline image data is non-trivial
   // but the test stays fast.
   img->FillRectangle("#ff0000", 0,  0, 16, 16);
   img->FillRectangle("#00ff00", 16, 0, 16, 16);
   img->FillRectangle("#0000ff", 0,  16, 16, 16);
   img->FillRectangle("#ffff00", 16, 16, 16, 16);

   TCanvas c("tasimage_pdf_canvas", "tasimage_pdf_canvas", 200, 200);
   img->Draw("X");
   c.SaveAs(pdfFile);
   delete img;

   FileStat_t st;
   ASSERT_EQ(gSystem->GetPathInfo(pdfFile, st), 0) << "PDF file was not created.";
   ASSERT_GT(st.fSize, 1024) << "PDF is suspiciously small.";

   const std::string pdf = SlurpFile(pdfFile);
   ASSERT_FALSE(pdf.empty());
   ASSERT_EQ(pdf.compare(0, 4, "%PDF"), 0) << "File does not look like a PDF.";

   const std::string decoded = DecodeAllFlateStreams(pdf);
   ASSERT_FALSE(decoded.empty()) << "No Flate streams could be decoded.";

   EXPECT_TRUE(ContainsOperator(decoded, "BI"))
      << "No 'BI' inline-image marker in any content stream — TASImage::Paint "
         "did not embed image data.";
   EXPECT_TRUE(ContainsOperator(decoded, "EI"))
      << "No 'EI' inline-image terminator in any content stream.";
   EXPECT_NE(decoded.find("/CS /RGB"), std::string::npos)
      << "Inline image colourspace declaration missing.";

   gSystem->Unlink(pdfFile);
}
