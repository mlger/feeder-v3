#ifndef FEEDER_UI_XML_H
#define FEEDER_UI_XML_H

/* XML source of a UI component by file name without ".xml", or NULL.
 * Implemented by ui_xml_gen.c, which tools/gen_ui_xml.py writes from hmi/ui/. */
const char *ui_xml_get(const char *name);

#endif
