#!/usr/bin/env python3
"""Build the player guide and code guidebook PDFs from project Markdown.

Requires reportlab, markdown-it-py and Pillow in the selected Python environment.
Run: python3 tools/make_guides.py
The firmware and connected board are never accessed by this tool.
"""
from __future__ import annotations

import argparse
from html import escape
from pathlib import Path
import re
import textwrap

try:
    from markdown_it import MarkdownIt
    from reportlab.graphics.shapes import Drawing, Line, Polygon, Rect, String
    from reportlab.lib import colors
    from reportlab.lib.enums import TA_CENTER, TA_LEFT
    from reportlab.lib.pagesizes import A4
    from reportlab.lib.styles import ParagraphStyle, getSampleStyleSheet
    from reportlab.pdfbase import pdfmetrics
    from reportlab.pdfbase.ttfonts import TTFont
    from reportlab.platypus import (BaseDocTemplate, Flowable, Frame, Image,
        KeepTogether, ListFlowable, ListItem, PageBreak, PageTemplate, Paragraph,
        Preformatted, Spacer, Table, TableStyle)
    from reportlab.platypus.tableofcontents import TableOfContents
except ImportError as error:
    raise SystemExit("Guide generation needs reportlab, markdown-it-py and Pillow.") from error

ROOT = Path(__file__).resolve().parents[1]
GREEN = colors.HexColor("#173f32")
ACCENT = colors.HexColor("#ba8735")
PALE = colors.HexColor("#eef4ee")
INK = colors.HexColor("#23312c")
MUTED = colors.HexColor("#56665d")
LINE = colors.HexColor("#ced9cf")
PAGE_W, PAGE_H = A4
MARGIN = 45
WIDTH = PAGE_W - MARGIN * 2


def register_fonts():
    candidates = Path('/System/Library/Fonts/Supplemental')
    fonts = [('Guide', 'Arial.ttf'), ('Guide-Bold', 'Arial Bold.ttf'),
             ('Guide-Italic', 'Arial Italic.ttf'), ('Guide-Code', 'Courier New.ttf')]
    if all((candidates / filename).exists() for _, filename in fonts):
        for name, filename in fonts:
            pdfmetrics.registerFont(TTFont(name, str(candidates / filename)))
        pdfmetrics.registerFontFamily('Guide', normal='Guide', bold='Guide-Bold',
                                     italic='Guide-Italic', boldItalic='Guide-Bold')
        return 'Guide', 'Guide-Bold', 'Guide-Code'
    return 'Helvetica', 'Helvetica-Bold', 'Courier'


FONT, BOLD, MONO = register_fonts()
STYLES = getSampleStyleSheet()
STYLES.add(ParagraphStyle('BodyGuide', fontName=FONT, fontSize=10.3, leading=14.8,
    textColor=INK, spaceAfter=7, allowWidows=0, allowOrphans=0))
STYLES.add(ParagraphStyle('GuideH1', fontName=BOLD, fontSize=17.5, leading=22,
    textColor=GREEN, spaceBefore=17, spaceAfter=9, keepWithNext=True))
STYLES.add(ParagraphStyle('GuideH2', parent=STYLES['GuideH1'], fontSize=12.8,
    leading=17, spaceBefore=12, spaceAfter=6))
STYLES.add(ParagraphStyle('GuideH3', parent=STYLES['GuideH2'], fontSize=11.2,
    leading=15))
STYLES.add(ParagraphStyle('GuideTitle', fontName=BOLD, fontSize=29, leading=35,
    textColor=GREEN, spaceAfter=13))
STYLES.add(ParagraphStyle('GuideSubtitle', fontName=FONT, fontSize=12.2,
    leading=18, textColor=MUTED, spaceAfter=14))
STYLES.add(ParagraphStyle('GuideCode', fontName=MONO, fontSize=8.2, leading=11,
    textColor=INK, backColor=PALE, borderColor=LINE, borderWidth=0.5,
    borderPadding=9, spaceBefore=5, spaceAfter=10))
STYLES.add(ParagraphStyle('GuideCaption', fontName=FONT, fontSize=8.5, leading=12,
    textColor=MUTED, spaceAfter=10, alignment=TA_CENTER))
STYLES.add(ParagraphStyle('GuideCell', parent=STYLES['BodyGuide'], fontSize=8.5,
    leading=11.5, spaceAfter=0))
STYLES.add(ParagraphStyle('GuideCellHead', parent=STYLES['GuideCell'],
    fontName=BOLD, textColor=colors.white))
STYLES.add(ParagraphStyle('GuideTOC', fontName=FONT, fontSize=10.5, leading=17,
    textColor=INK, leftIndent=0, firstLineIndent=0, spaceBefore=4))
STYLES.add(ParagraphStyle('GuideTOCSub', parent=STYLES['GuideTOC'], fontSize=9,
    leading=13, leftIndent=13, spaceBefore=1))


def box(drawing, x, y, w, h, lines, *, fill=PALE, size=9):
    drawing.add(Rect(x, y, w, h, rx=6, ry=6, fillColor=fill,
                     strokeColor=LINE, strokeWidth=0.8))
    for index, value in enumerate(lines):
        drawing.add(String(x + w / 2, y + h / 2 + (len(lines) - 1) * 6 - index * 12,
            value, fontName=BOLD if index == 0 else FONT, fontSize=size,
            fillColor=GREEN if index == 0 else INK, textAnchor='middle'))


def arrow(drawing, points, label=None, label_at=None):
    import math
    for (x1, y1), (x2, y2) in zip(points, points[1:]):
        drawing.add(Line(x1, y1, x2, y2, strokeColor=GREEN, strokeWidth=1.1))
    x, y = points[-1]
    previous = points[-2]
    angle = math.atan2(y - previous[1], x - previous[0])
    drawing.add(Polygon([x, y,
        x - 5 * math.cos(angle - .45), y - 5 * math.sin(angle - .45),
        x - 5 * math.cos(angle + .45), y - 5 * math.sin(angle + .45)],
        fillColor=GREEN, strokeColor=GREEN))
    if label:
        lx, ly = label_at or ((points[0][0] + x) / 2, (points[0][1] + y) / 2 + 5)
        drawing.add(String(lx, ly, label, fontName=FONT, fontSize=8,
                           fillColor=MUTED, textAnchor='middle'))


def architecture_diagram():
    d = Drawing(WIDTH, 253)
    box(d, 4, 191, 147, 48, ['Button and motion', 'EXTI and sensor readings'])
    box(d, 180, 191, 145, 48, ['main.c', 'Schedules and dispatches'])
    box(d, 354, 191, 146, 48, ['Telemetry', 'Checks physical units'])
    arrow(d, [(151, 215), (180, 215)])
    arrow(d, [(325, 215), (354, 215)])
    box(d, 4, 103, 147, 48, ['button.c', 'Edges -> SINGLE / DOUBLE'])
    box(d, 180, 103, 145, 48, ['game.c', 'Rules, state and events'])
    box(d, 354, 103, 146, 48, ['board_io.c', 'Sensors and queued UART'])
    arrow(d, [(77, 191), (77, 151)])
    arrow(d, [(151, 127), (180, 127)])
    arrow(d, [(252, 191), (252, 151)])
    arrow(d, [(427, 191), (427, 151)])
    box(d, 4, 15, 147, 48, ['UART and LED', 'Messages and mode pattern'])
    box(d, 180, 15, 145, 48, ['game_display.c', 'Original pixel renderer'])
    box(d, 354, 15, 146, 48, ['external_io.c', 'OLED, IR and D6 buzzer'])
    arrow(d, [(252, 103), (252, 63)], 'events and frame', (252, 81))
    arrow(d, [(205, 103), (205, 85), (77, 85), (77, 63)])
    arrow(d, [(300, 103), (300, 85), (427, 85), (427, 63)])
    return d


def state_diagram():
    d = Drawing(WIDTH, 243)
    box(d, 30, 163, 175, 54, ['STANDBY', 'Solid LED; singles ignored'])
    box(d, 302, 163, 175, 54, ['BATTLE', '1 Hz; charge and auto-fire'])
    box(d, 302, 34, 175, 54, ['ATTACKED BATTLE', '2 Hz; ten-second rescue'])
    box(d, 30, 34, 175, 54, ['TERMINATED', 'UART off; reset required'])
    arrow(d, [(205, 199), (302, 199)], 'double', (253, 205))
    arrow(d, [(302, 176), (205, 176)], 'double', (253, 162))
    arrow(d, [(438, 163), (438, 88)], 'inversion', (468, 122))
    arrow(d, [(335, 88), (335, 163)], 'double before 10 s', (285, 120))
    arrow(d, [(302, 61), (205, 61)], '10 s expires', (253, 70))
    arrow(d, [(76, 88), (76, 163)], 'reset', (60, 122))
    d.add(String(253, 8, 'Win/loss belongs to the game round; battle mode continues.',
        fontName=FONT, fontSize=8.5, fillColor=MUTED, textAnchor='middle'))
    return d


def sequence_diagram():
    d = Drawing(WIDTH, 277)
    positions = [45, 147, 248, 351, 456]
    labels = ['EXTI', 'Decoder', 'Game', 'Outputs', 'Laptop']
    for x, label in zip(positions, labels):
        box(d, x - 41, 237, 82, 31, [label], size=9)
        d.add(Line(x, 15, x, 237, strokeColor=LINE, strokeWidth=0.8,
                   strokeDashArray=[3, 3]))
    rows = [(0, 1, 209, 'timestamp queued'),
            (1, 2, 170, 'SINGLE at t + 500 ms'),
            (2, 3, 132, 'CHARGED; FIRED if energy >= 5'),
            (2, 3, 94, 'HIT and next TARGET'),
            (3, 4, 55, 'rendered frame read over USB')]
    for first, second, y, label in rows:
        arrow(d, [(positions[first], y), (positions[second], y)], label,
              ((positions[first] + positions[second]) / 2, y + 8))
    return d


def mirror_diagram():
    d = Drawing(WIDTH, 182)
    items = [('STM32 rules', 'Score, target, energy'), ('C renderer', '128 x 64 -> 1024 B'),
             ('ST-Link reader', '256 words; check seq'), ('Laptop window', 'Enlarge original pixels')]
    for index, lines in enumerate(items):
        x = index * 128
        box(d, x + 2, 91, 116, 57, list(lines), size=8)
        if index < 3:
            arrow(d, [(x + 118, 119), (x + 130, 119)])
    box(d, 60, 15, 385, 46, ['Coherent publication',
        'odd sequence -> write pixels -> even sequence; reader compares both ends'], size=8)
    return d


def diagram_for(source):
    if 'stateDiagram' in source:
        return state_diagram()
    if 'sequenceDiagram' in source:
        return sequence_diagram()
    if 'flowchart LR' in source and 'laptop_display' in source:
        return mirror_diagram()
    return architecture_diagram()


class GuideDocument(BaseDocTemplate):
    def __init__(self, destination, title, *, include_toc):
        super().__init__(str(destination), pagesize=A4, leftMargin=MARGIN,
            rightMargin=MARGIN, topMargin=48, bottomMargin=44,
            title=title, author='EE2028 Garden Guard project',
            subject='Player instructions and implementation study guide')
        self.guide_title = title
        self.include_toc = include_toc
        self.heading_index = 0
        frame = Frame(MARGIN, 44, WIDTH, PAGE_H - 92, leftPadding=0,
                      rightPadding=0, topPadding=0, bottomPadding=0)
        self.addPageTemplates(PageTemplate(id='guide', frames=frame, onPage=self.decorate))

    def beforeDocument(self):
        self.heading_index = 0

    def decorate(self, canvas, doc):
        canvas.saveState()
        canvas.setStrokeColor(LINE)
        canvas.setLineWidth(.5)
        canvas.line(MARGIN, PAGE_H - 30, PAGE_W - MARGIN, PAGE_H - 30)
        canvas.setFont(FONT, 8)
        canvas.setFillColor(MUTED)
        canvas.drawString(MARGIN, PAGE_H - 23, self.guide_title)
        canvas.drawRightString(PAGE_W - MARGIN, PAGE_H - 23, 'EE2028 Assignment 2')
        canvas.line(MARGIN, 30, PAGE_W - MARGIN, 30)
        canvas.drawString(MARGIN, 18, '24 October 2026 | D6 buzzer | 30 fps laptop target')
        canvas.drawRightString(PAGE_W - MARGIN, 18, str(doc.page))
        canvas.restoreState()

    def afterFlowable(self, flowable):
        if not isinstance(flowable, Paragraph):
            return
        if flowable.style.name not in ('GuideH1', 'GuideH2'):
            return
        level = 0 if flowable.style.name == 'GuideH1' else 1
        self.heading_index += 1
        key = 'section-' + str(self.heading_index)
        label = flowable.getPlainText()
        self.canv.bookmarkPage(key)
        self.canv.addOutlineEntry(label, key, level=level, closed=False)
        if self.include_toc:
            self.notify('TOCEntry', (level, label, self.page, key))


class MarkdownGuide:
    def __init__(self, source):
        self.source = source
        self.parser = MarkdownIt('commonmark').enable('table')

    def inline(self, tokens):
        parts = []
        for token in tokens or []:
            kind = token.type
            if kind == 'text':
                parts.append(escape(token.content))
            elif kind == 'code_inline':
                parts.append(f'<font name="{MONO}" size="9">{escape(token.content)}</font>')
            elif kind in ('strong_open', 'strong_close'):
                parts.append('<b>' if kind.endswith('open') else '</b>')
            elif kind in ('em_open', 'em_close'):
                parts.append('<i>' if kind.endswith('open') else '</i>')
            elif kind in ('softbreak', 'hardbreak'):
                parts.append(' ' if kind == 'softbreak' else '<br/>')
            elif kind == 'link_open':
                href = token.attrGet('href') or ''
                # Print source paths rather than embed machine-specific file URIs.
                parts.append(f'<font color="#173f32"><u>' if not href.startswith(('http://', 'https://'))
                             else f'<link href="{escape(href, quote=True)}" color="#173f32"><u>')
            elif kind == 'link_close':
                # Inline nesting for the links used by these guides is simple.
                opener = next((value for value in reversed(parts) if value.startswith(('<font color=', '<link '))), '')
                parts.append('</u></link>' if opener.startswith('<link ') else '</u></font>')
            elif kind == 'image':
                parts.append(escape(token.content))
        return ''.join(parts)

    @staticmethod
    def end_of(tokens, start):
        depth = 1
        kind = tokens[start].type.removesuffix('_open')
        for index in range(start + 1, len(tokens)):
            if tokens[index].type == kind + '_open':
                depth += 1
            elif tokens[index].type == kind + '_close':
                depth -= 1
                if depth == 0:
                    return index
        raise ValueError('Unclosed Markdown block: ' + kind)

    def table(self, tokens):
        rows, row, head = [], [], False
        for token in tokens:
            if token.type == 'thead_open':
                head = True
            elif token.type == 'thead_close':
                head = False
            elif token.type == 'tr_open':
                row = []
            elif token.type == 'inline':
                row.append(Paragraph(self.inline(token.children), STYLES['GuideCellHead' if head else 'GuideCell']))
            elif token.type == 'tr_close':
                rows.append(row)
        if not rows:
            return Spacer(1, 1)
        columns = len(rows[0])
        # File/role maps need more space for their first column's source paths.
        weights = [1] * columns
        if columns == 2:
            weights = [1, 1.8]
        elif columns == 4:
            weights = [1.15, 1.65, 1.65, 1]
        widths = [WIDTH * weight / sum(weights) for weight in weights]
        table = Table(rows, colWidths=widths, repeatRows=1, hAlign='LEFT',
                      spaceBefore=5, spaceAfter=11)
        table.setStyle(TableStyle([
            ('BACKGROUND', (0, 0), (-1, 0), GREEN),
            ('ROWBACKGROUNDS', (0, 1), (-1, -1), [colors.white, PALE]),
            ('VALIGN', (0, 0), (-1, -1), 'TOP'),
            ('LEFTPADDING', (0, 0), (-1, -1), 7),
            ('RIGHTPADDING', (0, 0), (-1, -1), 7),
            ('TOPPADDING', (0, 0), (-1, -1), 6),
            ('BOTTOMPADDING', (0, 0), (-1, -1), 6),
            ('LINEBELOW', (0, 0), (-1, 0), .7, GREEN),
            ('LINEBELOW', (0, 1), (-1, -1), .35, LINE),
        ]))
        return table

    def blocks(self, tokens, *, skip_title=False):
        story = []
        index = 0
        while index < len(tokens):
            token = tokens[index]
            kind = token.type
            if kind == 'heading_open':
                level = int(token.tag[1])
                if not (skip_title and level == 1):
                    style = STYLES['GuideH1' if level <= 2 else 'GuideH2' if level == 3 else 'GuideH3']
                    story.append(Paragraph(self.inline(tokens[index + 1].children), style))
                index += 3
            elif kind == 'paragraph_open':
                inline = tokens[index + 1]
                images = [item for item in (inline.children or []) if item.type == 'image']
                if images:
                    for item in images:
                        path = (self.source.parent / item.attrGet('src')).resolve()
                        if not path.is_file():
                            raise ValueError('Missing guide image: ' + str(path))
                        image = Image(str(path))
                        image.drawWidth = min(240, WIDTH)
                        image.drawHeight = image.drawWidth * image.imageHeight / image.imageWidth
                        image.hAlign = 'CENTER'
                        image._restrictSize(WIDTH, 175)
                        story.extend([image, Paragraph(escape(item.content), STYLES['GuideCaption'])])
                else:
                    story.append(Paragraph(self.inline(inline.children), STYLES['BodyGuide']))
                index += 3
            elif kind in ('bullet_list_open', 'ordered_list_open'):
                end = self.end_of(tokens, index)
                items = []
                cursor = index + 1
                while cursor < end:
                    if tokens[cursor].type == 'list_item_open':
                        item_end = self.end_of(tokens, cursor)
                        children = self.blocks(tokens[cursor + 1:item_end])
                        if children:
                            items.append(ListItem(children, leftIndent=10))
                        cursor = item_end + 1
                    else:
                        cursor += 1
                listing = ListFlowable(items, bulletType='1' if kind == 'ordered_list_open' else 'bullet',
                    start=(token.attrGet('start') or '1') if kind == 'ordered_list_open' else 'circle', leftIndent=15,
                    bulletFontName=FONT, bulletFontSize=9, spaceAfter=8)
                # ReportLab excludes list containers from keepWithNext. Expose
                # their generated item flowables so a heading stays with its
                # first item while the rest of a long list can still split.
                story.extend(listing.split(WIDTH, PAGE_H))
                index = end + 1
            elif kind == 'fence':
                if token.info.strip() == 'mermaid':
                    story.extend([Spacer(1, 4), diagram_for(token.content), Spacer(1, 9)])
                else:
                    lines = []
                    for line in token.content.rstrip().splitlines():
                        lines.extend(textwrap.wrap(line, 91, replace_whitespace=False,
                            drop_whitespace=False, break_long_words=True,
                            break_on_hyphens=False) or [''])
                    story.append(Preformatted('\n'.join(lines), STYLES['GuideCode']))
                index += 1
            elif kind == 'table_open':
                end = self.end_of(tokens, index)
                story.append(self.table(tokens[index:end + 1]))
                index = end + 1
            elif kind == 'blockquote_open':
                end = self.end_of(tokens, index)
                story.extend(self.blocks(tokens[index + 1:end]))
                index = end + 1
            elif kind == 'hr':
                story.append(Spacer(1, 9))
                index += 1
            else:
                index += 1
        return story


def build_guide(source, destination, *, book):
    content = source.read_text()
    title_match = re.search(r'^#\s+(.+)$', content, re.MULTILINE)
    title = title_match.group(1) if title_match else source.stem
    destination.parent.mkdir(parents=True, exist_ok=True)
    doc = GuideDocument(destination, title, include_toc=book)
    story = [Spacer(1, 20 if book else 7), Paragraph(escape(title), STYLES['GuideTitle']),
             Paragraph('EE2028 Assignment 2 | STM32L4S5 | Updated 24 October 2026', STYLES['GuideSubtitle'])]
    if book:
        story.extend([Paragraph('Understand how the firmware turns physical input into game decisions, '
            'UART messages, sound and live laptop pixels.', STYLES['BodyGuide']), Spacer(1, 22)])
        art = ROOT / 'output/game-preview/native/standby-0.png'
        if art.exists():
            image = Image(str(art), width=400, height=200)
            image.hAlign = 'CENTER'
            story.extend([image, Spacer(1, 14)])
        story.extend([Paragraph('Read the player guide first. Then follow the input-to-shot example '
            'and use the file map to navigate the source. Source paths are relative to the Assignment 2 folder.',
            STYLES['BodyGuide']), Spacer(1, 18),
            Paragraph('Current setup: the board owns the game, the D6 buzzer supplies sound, '
            'and the laptop mirrors the original 128 by 64 framebuffer with a 30 fps target.',
            STYLES['BodyGuide']), PageBreak(),
            Paragraph('Contents', STYLES['GuideTitle'])])
        toc = TableOfContents()
        toc.levelStyles = [STYLES['GuideTOC'], STYLES['GuideTOCSub']]
        toc.dotsMinLevel = 0
        story.extend([toc, PageBreak()])
    renderer = MarkdownGuide(source)
    story.extend(renderer.blocks(renderer.parser.parse(content), skip_title=True))
    doc.multiBuild(story) if book else doc.build(story)
    print(destination)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--output', type=Path, default=ROOT / 'output/pdf')
    args = parser.parse_args()
    build_guide(ROOT / 'docs/how-to-play.md', args.output / 'Garden_Guard_Player_Guide.pdf', book=False)
    build_guide(ROOT / 'docs/code-guidebook.md', args.output / 'Garden_Guard_Code_Guidebook.pdf', book=True)


if __name__ == '__main__':
    main()
