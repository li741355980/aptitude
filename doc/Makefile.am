EXTRA_DIST=aptitude-common.xsl aptitude-html.xsl aptitude-man.xsl aptitude-txt.xsl aptitude.css fixup-text html-to-text

if USE_PO4A

dist distdir: po4a-dist

update-po: po4a-dist

po4a-dist:
	-@cd $(srcdir) && po4a -M utf-8 -k 0 po4a/po4a.cfg

PO4A_SUBDIRS=de es pl
endif

SUBDIRS=$(PO4A_SUBDIRS) cs en fi fr ja po4a
