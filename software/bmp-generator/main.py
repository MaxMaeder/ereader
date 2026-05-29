import argparse
from pathlib import Path

import ebooklib
from ebooklib import epub
from bs4 import BeautifulSoup
from PIL import Image, ImageDraw, ImageFont

DISPLAY_WIDTH = 552
DISPLAY_HEIGHT = 960

MARGIN = 20
FONT_SIZE = 24
LINE_SPACING = 4

FONT_PATH = Path(__file__).parent / "georgia.ttf"

PARAGRAPH_TAGS = {"p", "h1", "h2", "h3", "h4", "h5", "h6", "li", "blockquote"}


def extract_paragraphs_from_html(html: bytes) -> list[str]:
    """Extract paragraphs from HTML, preserving natural text flow."""
    soup = BeautifulSoup(html, "html.parser")

    body = soup.find("body") or soup
    paragraphs = []

    # Prefer leaf-level paragraph elements
    elements = body.find_all(PARAGRAPH_TAGS)
    if elements:
        for element in elements:
            # Skip if this element contains other block elements (it's a wrapper)
            if element.find(PARAGRAPH_TAGS):
                continue
            text = element.get_text(separator=" ", strip=True)
            text = " ".join(text.split())
            if text:
                paragraphs.append(text)
    else:
        # Fallback: treat the whole body as one block
        text = body.get_text(separator=" ", strip=True)
        text = " ".join(text.split())
        if text:
            paragraphs.append(text)

    return paragraphs


def extract_text_from_epub(epub_path: str) -> list[list[str]]:
    """Extract paragraphs from each chapter in the EPUB."""
    book = epub.read_epub(epub_path)
    chapters = []

    for item in book.get_items_of_type(ebooklib.ITEM_DOCUMENT):
        paragraphs = extract_paragraphs_from_html(item.get_content())
        if paragraphs:
            chapters.append(paragraphs)

    return chapters


def wrap_paragraph(text: str, draw: ImageDraw.ImageDraw, font: ImageFont.FreeTypeFont, max_width: int) -> list[str]:
    """Word-wrap a paragraph to fit within max_width pixels."""
    words = text.split()
    if not words:
        return [""]

    lines = []
    current = words[0]

    for word in words[1:]:
        test = f"{current} {word}"
        bbox = draw.textbbox((0, 0), test, font=font)
        if bbox[2] - bbox[0] <= max_width:
            current = test
        else:
            lines.append(current)
            current = word

    lines.append(current)
    return lines


def paginate_chapters(chapters: list[list[str]], draw: ImageDraw.ImageDraw, font: ImageFont.FreeTypeFont) -> list[list[str]]:
    """Convert chapters of paragraphs into pages of lines.

    Each chapter starts on a new page, and blank lines between paragraphs
    are preserved within pages but stripped from page tops.
    """
    usable_width = DISPLAY_WIDTH - 2 * MARGIN
    usable_height = DISPLAY_HEIGHT - 2 * MARGIN

    bbox = draw.textbbox((0, 0), "Ag", font=font)
    line_height = (bbox[3] - bbox[1]) + LINE_SPACING
    lines_per_page = int(usable_height // line_height)

    pages = []

    for paragraphs in chapters:
        # Build all lines for this chapter
        chapter_lines = []
        for para in paragraphs:
            wrapped = wrap_paragraph(para, draw, font, usable_width)
            chapter_lines.extend(wrapped)
            chapter_lines.append("")

        # Strip trailing blank lines from chapter
        while chapter_lines and chapter_lines[-1] == "":
            chapter_lines.pop()

        # Paginate this chapter's lines
        current_page = []
        for line in chapter_lines:
            if len(current_page) >= lines_per_page:
                pages.append(current_page)
                current_page = []
                # Don't start a new page with a blank line
                if line == "":
                    continue
            current_page.append(line)

        if current_page:
            pages.append(current_page)

    return pages


def render_page(lines: list[str], font: ImageFont.FreeTypeFont) -> Image.Image:
    """Render a page of lines as a 1-bit BMP image."""
    img = Image.new("1", (DISPLAY_WIDTH, DISPLAY_HEIGHT), color=1)
    draw = ImageDraw.Draw(img)

    bbox = draw.textbbox((0, 0), "Ag", font=font)
    line_height = (bbox[3] - bbox[1]) + LINE_SPACING

    y = MARGIN
    for line in lines:
        draw.text((MARGIN, y), line, font=font, fill=0)
        y += line_height

    return img.transpose(Image.Transpose.ROTATE_90)


def main():
    parser = argparse.ArgumentParser(description="Convert EPUB to BMP images for e-reader display")
    parser.add_argument("epub", nargs="?", default="book.epub", help="Path to EPUB file (default: book.epub)")
    parser.add_argument("-n", "--num-pages", type=int, default=None, help="Number of pages to render (default: all)")
    parser.add_argument("-o", "--output", default="output", help="Output directory (default: output)")
    args = parser.parse_args()

    epub_path = Path(args.epub)
    if not epub_path.exists():
        print(f"Error: '{args.epub}' not found")
        return 1

    output_dir = Path(args.output)
    output_dir.mkdir(parents=True, exist_ok=True)

    print(f"Reading '{args.epub}'...")
    chapters = extract_text_from_epub(args.epub)
    print(f"Found {len(chapters)} chapters")

    font = ImageFont.truetype(str(FONT_PATH), FONT_SIZE)

    scratch = Image.new("1", (DISPLAY_WIDTH, DISPLAY_HEIGHT), color=1)
    scratch_draw = ImageDraw.Draw(scratch)

    pages = paginate_chapters(chapters, scratch_draw, font)

    if args.num_pages is not None:
        pages = pages[: args.num_pages]

    print(f"Rendering {len(pages)} pages at {DISPLAY_WIDTH}x{DISPLAY_HEIGHT} (1-bit BW)...")

    for i, page_lines in enumerate(pages):
        img = render_page(page_lines, font)
        out_path = output_dir / f"page_{i:04d}.bmp"
        img.save(out_path, format="BMP")

    print(f"Done! {len(pages)} BMP images saved to '{output_dir}/'")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
