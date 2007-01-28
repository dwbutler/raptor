/* -*- Mode: c; c-basic-offset: 2 -*-
 *
 * raptor_grddl.c - Raptor GRDDL Parser implementation
 *
 * Copyright (C) 2005-2006, David Beckett http://purl.org/net/dajobe/
 * Copyright (C) 2005, University of Bristol, UK http://www.bristol.ac.uk/
 * 
 * This package is Free Software and part of Redland http://librdf.org/
 * 
 * It is licensed under the following three licenses as alternatives:
 *   1. GNU Lesser General Public License (LGPL) V2.1 or any newer version
 *   2. GNU General Public License (GPL) V2 or any newer version
 *   3. Apache License, V2.0 or any newer version
 * 
 * You may not use this file except in compliance with at least one of
 * the above three licenses.
 * 
 * See LICENSE.html or LICENSE.txt at the top of this package for the
 * complete terms and further detail along with the license texts for
 * the licenses in COPYING.LIB, COPYING and LICENSE-2.0.txt respectively.
 *
 */

/*
 * Specification:
 *   Gleaning Resource Descriptions from Dialects of Languages (GRDDL)
 *   W3C Working Draft 24 October 2006
 *   http://www.w3.org/TR/2006/WD-grddl-20061024/
 *   http://www.w3.org/TR/grddl/
 */


#ifdef HAVE_CONFIG_H
#include <raptor_config.h>
#endif

#ifdef WIN32
#include <win32_raptor_config.h>
#endif

#include <stdio.h>
#include <string.h>
#include <ctype.h>
#include <stdarg.h>
#ifdef HAVE_ERRNO_H
#include <errno.h>
#endif
#ifdef HAVE_STDLIB_H
#include <stdlib.h>
#endif

/* Raptor includes */
#include "raptor.h"
#include "raptor_internal.h"

#include <libxml/xpathInternals.h>

#include <libxslt/xslt.h>
#include <libxslt/xsltInternals.h>
#include <libxslt/transform.h>
#include <libxslt/xsltutils.h>


/*
 * libxslt API notes
 *
 * Inputs to an XSLT transformation process with libxslt are:
 *   1. A set of (key:value) parameters.
 *   2. An xsltStylesheetPtr for the XSLT sheet
 *     Which could be made from a file or an xmlDoc; and the xmlDoc.
 *     made from a file or memory buffer.
 *   3. An xmlDoc for the XML source
 *     Which could be made from a file or a memory buffer.
 *   
 */



/*
 * XSLT parser object
 */
struct raptor_grddl_parser_context_s {
  xmlSAXHandler sax;

  /* XML document ctxt */
  xmlParserCtxtPtr ctxt;

  /* Create xpath evaluation context */
  xmlXPathContextPtr xpathCtx; 

  /* parser for dealing with the result */
  raptor_parser* internal_parser;
  /* ... constructed with this name */
  const char* internal_parser_name;

  /* sax2 structure - only for recording error pointers */
  raptor_sax2* sax2;

  /* URI of root namespace of document */
  raptor_uri* root_ns_uri;

  /* List of transformation URIs for document */
  raptor_sequence* doc_transform_uris;

  /* Copy of the user data statement_handler overwritten to point to
   * raptor_grddl_relay_triples() 
   */
  void* saved_user_data;
  raptor_statement_handler saved_statement_handler;

  /* URI data-view:namespaceTransformation */
  raptor_uri* namespace_transformation_uri;

  /* URI data-view:profileTransformation */
  raptor_uri* profile_transformation_uri;

  /* List of namespace / <head profile> URIs */
  raptor_sequence* profile_uris;

  /* List of visited URIs */
  raptor_sequence* visited_uris;

  /* Depth of GRDDL parsers - 0 means that the lists above 
   * are owned by this parser: visited_uris
   * */
  int grddl_depth;
};


typedef struct raptor_grddl_parser_context_s raptor_grddl_parser_context;


static int
raptor_grddl_parse_init(raptor_parser* rdf_parser, const char *name)
{
  raptor_grddl_parser_context* grddl_parser=(raptor_grddl_parser_context*)rdf_parser->context;

  /* sax2 structure - only for recording error pointers */
  grddl_parser->sax2=raptor_new_sax2(rdf_parser,
                                    rdf_parser,
                                    raptor_parser_error_message_handler,
                                    rdf_parser,
                                    raptor_parser_fatal_error_message_handler,
                                    rdf_parser,
                                    raptor_parser_warning_message_handler);
  /* FIXME: this should really be part of raptor_new_sax2() 
   * since every use of the above calls the following soon after
   */
  raptor_sax2_set_locator(grddl_parser->sax2, &rdf_parser->locator);
  
  /* The following error fields are normally initialised by
   * raptor_libxml_init() via raptor_sax2_parse_start() which is
   * not used here as we go to libxml calls direct.
   */
  raptor_libxml_init_sax_error_handlers(&grddl_parser->sax);
  raptor_libxml_init_generic_error_handlers(grddl_parser->sax2);

  /* Sequence of URIs of XSLT sheets to transform the document */
  grddl_parser->doc_transform_uris=raptor_new_sequence((raptor_sequence_free_handler*)raptor_free_uri, (raptor_sequence_print_handler*)raptor_sequence_print_uri);

  /* Sequence of URIs from <head profile> */
  grddl_parser->profile_uris=raptor_new_sequence((raptor_sequence_free_handler*)raptor_free_uri, (raptor_sequence_print_handler*)raptor_sequence_print_uri);

  grddl_parser->namespace_transformation_uri=raptor_new_uri((const unsigned char*)"http://www.w3.org/2003/g/data-view#namespaceTransformation");
  grddl_parser->profile_transformation_uri=raptor_new_uri((const unsigned char*)"http://www.w3.org/2003/g/data-view#profileTransformation");

  /* Sequence of URIs visited - may be overwritten if this is not
   * the depth 0 grddl parser
   */
  grddl_parser->visited_uris=raptor_new_sequence((raptor_sequence_free_handler*)raptor_free_uri, (raptor_sequence_print_handler*)raptor_sequence_print_uri);

  return 0;
}


static void
raptor_grddl_parse_terminate(raptor_parser *rdf_parser)
{
  raptor_grddl_parser_context *grddl_parser=(raptor_grddl_parser_context*)rdf_parser->context;
  if(grddl_parser->ctxt) {
    if(grddl_parser->ctxt->myDoc) {
      xmlFreeDoc(grddl_parser->ctxt->myDoc);
      grddl_parser->ctxt->myDoc=NULL;
    }
    xmlFreeParserCtxt(grddl_parser->ctxt);
  }

  if(grddl_parser->xpathCtx)
    xmlXPathFreeContext(grddl_parser->xpathCtx);

  if(grddl_parser->internal_parser)
    raptor_free_parser(grddl_parser->internal_parser);

  if(grddl_parser->sax2)
    raptor_free_sax2(grddl_parser->sax2);

  if(grddl_parser->root_ns_uri)
    raptor_free_uri(grddl_parser->root_ns_uri);

  if(grddl_parser->doc_transform_uris)
    raptor_free_sequence(grddl_parser->doc_transform_uris);

  if(grddl_parser->profile_uris)
    raptor_free_sequence(grddl_parser->profile_uris);

  if(grddl_parser->namespace_transformation_uri)
    raptor_free_uri(grddl_parser->namespace_transformation_uri);

  if(grddl_parser->profile_transformation_uri)
    raptor_free_uri(grddl_parser->profile_transformation_uri);

  if(!grddl_parser->grddl_depth) {
    if(grddl_parser->visited_uris)
      raptor_free_sequence(grddl_parser->visited_uris);
  }
}


static void
raptor_grddl_parser_add_parent(raptor_parser *rdf_parser, 
                               raptor_grddl_parser_context* parent_grddl_parser)
{
  raptor_grddl_parser_context* grddl_parser=(raptor_grddl_parser_context*)rdf_parser->context;
  /* free any sequence here */
  if(grddl_parser->visited_uris)
    raptor_free_sequence(grddl_parser->visited_uris);

  /* share parent's list and do not free it here */
  grddl_parser->visited_uris= parent_grddl_parser->visited_uris;
  grddl_parser->grddl_depth= parent_grddl_parser->grddl_depth+1;
}

    

static int
raptor_grddl_parse_start(raptor_parser *rdf_parser) 
{
  raptor_locator *locator=&rdf_parser->locator;
  
  locator->line=1;

  return 0;
}


#define MATCH_IS_VALUE_LIST 1
#define MATCH_IS_PROFILE    2
static struct {
  const xmlChar* xpath;
  int flags;
  const xmlChar* xslt_sheet_uri;
} match_table[]={
  /* XHTML document where the GRDDL profile is in
   * <link ref='transform' href='url'>  inside the html <head>
   */
  {
    (const xmlChar*)"/html:html/html:head[contains(@profile,\"http://www.w3.org/2003/g/data-view\")]/html:link[@rel=\"transformation\"]/@href",
    0,
    NULL
  }
  ,
  /* XHTML document where the GRDDL profile is in
   * <a rel='transform' href='url'> inside the html <body>
   */
  {
    (const xmlChar*)"/html:html/html:head[contains(@profile,\"http://www.w3.org/2003/g/data-view\")]/../..//html:a[@rel=\"transformation\"]/@href",
    0,
    NULL
  }
  ,
  /* XML document linking to transform via attribute dataview:transformation 
   * Example: http://www.w3.org/2004/01/rdxh/grddl-p3p-example
   **/
  {
    (const xmlChar*)"//@dataview:transformation",
    MATCH_IS_VALUE_LIST,
    NULL
  }
  ,
#if 0
  /* FIXME Disabled. This returns the wrong namespaces in
   * Id: dc-extract.xsl,v 1.10 2005/09/07 17:10:06 connolly Exp
   */

  /* Dublin Core in <meta> tags http://dublincore.org/documents/dcq-html/ */
  {
    (const xmlChar*)"/html:html/html:head/html:link[@href=\"http://purl.org/dc/elements/1.1/\"]",
    0,
    (const xmlChar*)"http://www.w3.org/2000/06/dc-extract/dc-extract.xsl"
  }
  ,
#endif
#if 0
  /* Embedded RDF 
   * <head profile="http://purl.org/NET/erdf/profile"> inside <html>
   */
  { 
    (const xmlChar*)"/html:html/html:head[contains(@profile,\"http://purl.org/NET/erdf/profile\")]",
    0,
    (const xmlChar*)"http://purl.org/NET/erdf/extract-rdf.xsl"
  }
  ,
#endif
#if 0
  /* hCalendar microformat http://microformats.org/wiki/hcalendar */
  {
    (const xmlChar*)"//*[@class=\"vevent\"]",
    0,
    (const xmlChar*)"http://www.w3.org/2002/12/cal/glean-hcal.xsl"
  }
  ,
#endif
  { 
    NULL,
    0,
    0
  }
};


static const char* grddl_namespace_uris_ignore_list[]={
  "http://www.w3.org/1999/xhtml",
  "http://www.w3.org/1999/02/22-rdf-syntax-ns#",
  "http://www.w3.org/2001/XMLSchema",
  NULL
};


static void
raptor_grddl_relay_triples(void *user_data, const raptor_statement *statement)
{
  raptor_parser* rdf_parser=(raptor_parser*)user_data;
  raptor_grddl_parser_context* grddl_parser;

  grddl_parser=(raptor_grddl_parser_context*)rdf_parser->context;

  /* Look for a triple <uri> <uri> <uri> */
  if(statement->subject_type == RAPTOR_IDENTIFIER_TYPE_RESOURCE &&
     statement->predicate_type == RAPTOR_IDENTIFIER_TYPE_RESOURCE &&
     statement->object_type == RAPTOR_IDENTIFIER_TYPE_RESOURCE) {
    int i;
    raptor_uri* predicate_uri=grddl_parser->namespace_transformation_uri;

#ifdef RAPTOR_DEBUG
    RAPTOR_DEBUG2("Parser %p Relaying statement: ", rdf_parser);
    raptor_print_statement(statement, stderr);
    fputc('\n', stderr);
#endif

    /* Look for (i=0, root namespace URI)
     * <document-root-element-namespace-URI> data-view:namespaceTransformation ?tr
     * or (i>0, profile URIs)
     * <document-root-element-namespace-URI> data-view:profileTransformation ?tr
     * and then ?tr becomes a new document transformation URI
     */
    for(i=0; i < raptor_sequence_size(grddl_parser->profile_uris); i++) {
      raptor_uri* profile_uri=raptor_sequence_get_at(grddl_parser->profile_uris, i);
      if(i==1)
        predicate_uri=grddl_parser->profile_transformation_uri;

      if(!profile_uri)
        continue;
      
      if(raptor_uri_equals((raptor_uri*)statement->subject, profile_uri) &&
         raptor_uri_equals((raptor_uri*)statement->predicate, predicate_uri)) {
        raptor_uri* uri=(raptor_uri*)statement->object;
        
        RAPTOR_DEBUG2("Matches profile URI '%s'\n",
                      raptor_uri_as_string(profile_uri));

        /* add object as URI to transformation URI*/
        RAPTOR_DEBUG2("Found document transformation URI '%s'\n",
                      raptor_uri_as_string(uri));
        raptor_sequence_push(grddl_parser->doc_transform_uris, 
                             raptor_uri_copy(uri));
      } else {
        RAPTOR_DEBUG2("Does not match profile URI '%s'\n",
                      raptor_uri_as_string(profile_uri));
      }
      
    }

  }

  /* Send triple onwards to the original caller */
  if(grddl_parser->saved_statement_handler)
    grddl_parser->saved_statement_handler(grddl_parser->saved_user_data, 
                                         statement);
}


static int
raptor_grddl_ensure_internal_parser(raptor_parser* rdf_parser,
                                    const char* parser_name, int relay) 
{
  raptor_grddl_parser_context* grddl_parser=(raptor_grddl_parser_context*)rdf_parser->context;

  if(!grddl_parser->internal_parser_name ||
     strcmp(grddl_parser->internal_parser_name, parser_name)) {
    /* construct a new parser if none in use or not what is required */
    if(grddl_parser->internal_parser) {
      RAPTOR_DEBUG2("Freeing internal %s parser.\n",
                    grddl_parser->internal_parser_name);
      
      raptor_free_parser(grddl_parser->internal_parser);
      grddl_parser->internal_parser=NULL;
      grddl_parser->internal_parser_name=NULL;
    }
    
    RAPTOR_DEBUG2("Allocating new internal %s parser.\n", parser_name);
    grddl_parser->internal_parser=raptor_new_parser(parser_name);
    if(!grddl_parser->internal_parser) {
      raptor_parser_error(rdf_parser, "Failed to create %s parser",
                          parser_name);
      return 1;
    } else {
      /* initialise the new parser with the outer state */
      grddl_parser->internal_parser_name=parser_name;
      raptor_parser_copy_user_state(grddl_parser->internal_parser,
                                    rdf_parser);
      grddl_parser->saved_user_data=rdf_parser->user_data;
      grddl_parser->saved_statement_handler=rdf_parser->statement_handler;
    }
  }

  if(relay) {
    /* Go via relay */
    grddl_parser->internal_parser->user_data=rdf_parser;
    grddl_parser->internal_parser->statement_handler=raptor_grddl_relay_triples;
  } else {
    /* Or go direct to user handler */
    grddl_parser->internal_parser->user_data=grddl_parser->saved_user_data;
    grddl_parser->internal_parser->statement_handler=grddl_parser->saved_statement_handler;
  }
  return 0;
}


/* Run a GRDDL transform using a pre-parsed XSLT stylesheet already
 * formed into a libxml document (with URI)
 */
static int
raptor_grddl_run_grddl_transform_doc(raptor_parser* rdf_parser,
                                     raptor_uri* xslt_uri, xmlDocPtr xslt_doc,
                                     xmlDocPtr doc)
{
  raptor_grddl_parser_context* grddl_parser=(raptor_grddl_parser_context*)rdf_parser->context;
  int ret=0;
  xsltStylesheetPtr sheet=NULL;
  xmlDocPtr res=NULL;
  xmlChar *doc_txt=NULL;
  int doc_txt_len=0;
  const char* parser_name;

  RAPTOR_DEBUG3("Running GRDDL transform with XSLT URI '%s' on doc URI '%s'\n",
                raptor_uri_as_string(xslt_uri),
                raptor_uri_as_string(rdf_parser->base_uri));
  
  sheet = xsltParseStylesheetDoc(xslt_doc);
  if(!sheet) {
    raptor_parser_error(rdf_parser, "Failed to parse stylesheet in '%s'",
                        raptor_uri_as_string(xslt_uri));
    ret=1;
    goto cleanup_xslt;
  }
  
  res = xsltApplyStylesheet(sheet, doc, NULL); /* no params */
  if(!res) {
    raptor_parser_error(rdf_parser, "Failed to apply stylesheet in '%s'",
                        raptor_uri_as_string(xslt_uri));
    ret=1;
    goto cleanup_xslt;
  }

  if(res->type == XML_HTML_DOCUMENT_NODE) {
    if(sheet->method != NULL)
      xmlFree(sheet->method);
    sheet->method = xmlMalloc(5);
    strncpy((char*)sheet->method, "html", 5);
  }

  /* write the resulting XML to a string */
  xsltSaveResultToString(&doc_txt, &doc_txt_len, res, sheet);
  
  if(!doc_txt || !doc_txt_len) {
    /* FIXME: continue with an empty document? */
    raptor_parser_warning(rdf_parser, "XSLT returned an empty document");
    goto cleanup_xslt;
  }

  RAPTOR_DEBUG4("XSLT returned %d bytes document method %s media type %s\n",
                doc_txt_len,
                (sheet->method ? (const char*)sheet->method : "NULL"),
                (sheet->mediaType ? (const char*)sheet->mediaType : "NULL"));

  /* FIXME: Assumes mime types for XSLT <xsl:output method> */
  if(sheet->mediaType == NULL && sheet->method) {
    if(!(strcmp((const char*)sheet->method, "text"))) {
      sheet->mediaType = xmlMalloc(11);
      strncpy((char*)sheet->mediaType, "text/plain",11);
    } else if(!(strcmp((const char*)sheet->method, "xml"))) {
      sheet->mediaType = xmlMalloc(16);
      strncpy((char*)sheet->mediaType, "application/xml",16);
    } else if(!(strcmp((const char*)sheet->method, "html"))) {
      sheet->mediaType = xmlMalloc(10);
      /* FIXME: use xhtml mime type? */
      strncpy((char*)sheet->mediaType, "text/html",10);
    }
  }

  /* FIXME: Assume all that all media XML is RDF/XML and also that
   * with no information at all we have RDF/XML
   */
  if(!sheet->mediaType ||
     (sheet->mediaType &&
      !strcmp((const char*)sheet->mediaType, "application/xml"))) {
    if(sheet->mediaType)
      xmlFree(sheet->mediaType);
    sheet->mediaType = xmlMalloc(20);
    strncpy((char*)sheet->mediaType, "application/rdf+xml",20);
  }
  
  parser_name=raptor_guess_parser_name(NULL, (const char*)sheet->mediaType,
                                       doc_txt, doc_txt_len, NULL);
  RAPTOR_DEBUG3("Guessed parser %s from mime type '%s'\n",
                parser_name, sheet->mediaType);

  if(!strcmp((const char*)parser_name, "grddl")) {
    RAPTOR_DEBUG1("Ignoring guess to run grddl parser.");
    goto cleanup_xslt;
  }

  ret=raptor_grddl_ensure_internal_parser(rdf_parser, parser_name, 1);
  if(ret)
    goto cleanup_xslt;
  
  if(grddl_parser->internal_parser) {
    /* generate the triples */
    raptor_start_parse(grddl_parser->internal_parser, rdf_parser->base_uri);
    raptor_parse_chunk(grddl_parser->internal_parser, doc_txt, doc_txt_len, 1);
  }
  
  cleanup_xslt:
  if(doc_txt)    
    xmlFree(doc_txt);
  
  if(res)
    xmlFreeDoc(res);
  
  if(sheet)
    xsltFreeStylesheet(sheet);
  
  return ret;
}


typedef struct 
{
  raptor_parser* rdf_parser;
  xmlParserCtxtPtr xc;
} raptor_grddl_xml_parse_bytes_context;
  

static void
raptor_grddl_uri_xml_parse_bytes(raptor_www* www,
                                 void *userdata,
                                 const void *ptr, size_t size, size_t nmemb)
{
  raptor_grddl_xml_parse_bytes_context* xpbc=(raptor_grddl_xml_parse_bytes_context*)userdata;
  int len=size*nmemb;
  int rc=0;
  
  if(!xpbc->xc) {
    xmlParserCtxtPtr xc;
    
    xc = xmlCreatePushParserCtxt(NULL, NULL,
                                 (const char*)ptr, len,
                                 (const char*)raptor_uri_as_string(www->uri));
    if(!xc)
      rc=1;
    else {
      int libxml_options = 0;

#ifdef XML_PARSE_NONET
      if(xpbc->rdf_parser->features[RAPTOR_FEATURE_NO_NET])
        libxml_options |= XML_PARSE_NONET;
#endif
#ifdef HAVE_XMLCTXTUSEOPTIONS
      xmlCtxtUseOptions(xc, libxml_options);
#endif

      xc->replaceEntities = 1;
      xc->loadsubset = 1;
    }
    xpbc->xc=xc;
  } else
    rc=xmlParseChunk(xpbc->xc, (const char*)ptr, len, 0);

  if(rc)
    raptor_parser_error(xpbc->rdf_parser, "XML Parsing failed");
}


static int
raptor_grddl_fetch_uri(raptor_parser* rdf_parser, 
                       raptor_uri* uri,
                       raptor_www_write_bytes_handler write_bytes_handler,
                       void* write_bytes_user_data)
{
  raptor_www *www;
  const char *accept_h;
  int ret=0;
  
  if(rdf_parser->features[RAPTOR_FEATURE_NO_NET])
    return 1;
  
  www=raptor_www_new();
  if(!www)
    return 1;
  
  raptor_www_set_user_agent(www, "grddl/0.1");
  
  accept_h=raptor_parser_get_accept_header(rdf_parser);
  if(accept_h) {
    raptor_www_set_http_accept(www, accept_h);
    RAPTOR_FREE(cstring, accept_h);
  }
  if(rdf_parser->uri_filter)
    raptor_www_set_uri_filter(www, rdf_parser->uri_filter,
                              rdf_parser->uri_filter_user_data);
  raptor_www_set_error_handler(www, rdf_parser->error_handler, 
                               rdf_parser->error_user_data);
  raptor_www_set_write_bytes_handler(www, write_bytes_handler,
                                     write_bytes_user_data);
  
  ret=raptor_www_fetch(www, uri);
  
  raptor_www_free(www);

  return ret;
}


/* Run a GRDDL transform using a XSLT stylesheet at a given URI */
static int
raptor_grddl_run_grddl_transform_uri(raptor_parser* rdf_parser,
                                     raptor_uri* xslt_uri, xmlDocPtr doc)
{
  raptor_grddl_parser_context* grddl_parser;
  xmlParserCtxtPtr xslt_ctxt=NULL;
  raptor_grddl_xml_parse_bytes_context xpbc;
  int ret=0;

  grddl_parser=(raptor_grddl_parser_context*)rdf_parser->context;
  
  RAPTOR_DEBUG2("Running GRDDL transform with XSLT URI '%s'\n",
                raptor_uri_as_string(xslt_uri));
  
  /* make an xsltStylesheetPtr via the raptor_grddl_uri_xml_parse_bytes 
   * callback as bytes are returned
   */
  xpbc.xc=NULL;
  xpbc.rdf_parser=rdf_parser;

  ret=raptor_grddl_fetch_uri(rdf_parser,
                             xslt_uri,
                             raptor_grddl_uri_xml_parse_bytes,
                             &xpbc);
  if(ret) {
    RAPTOR_DEBUG2("Fetching XSLT document URI '%s' failed\n",
                raptor_uri_as_string(xslt_uri));
  } else {
    xslt_ctxt=xpbc.xc;
    xmlParseChunk(xpbc.xc, NULL, 0, 1);
  
    ret=raptor_grddl_run_grddl_transform_doc(rdf_parser,
                                             xslt_uri, xslt_ctxt->myDoc,
                                             doc);
  }
  
  if(xslt_ctxt)
    xmlFreeParserCtxt(xslt_ctxt); 
  
  return ret;
}


static int
raptor_grddl_seen_uri(raptor_grddl_parser_context* grddl_parser,
                      raptor_uri* uri)
{
  int i;
  int seen=0;
  raptor_sequence* seq=grddl_parser->visited_uris;
  
  for(i=0; i < raptor_sequence_size(seq); i++) {
    raptor_uri* vuri=(raptor_uri*)raptor_sequence_get_at(seq, i);
    if(raptor_uri_equals(uri, vuri)) {
      seen=1;
      break;
    }
  }

  if(seen)
    RAPTOR_DEBUG2("Already seen URI '%s'\n", raptor_uri_as_string(uri));
  
  return seen;
}


static void
raptor_grddl_done_uri(raptor_grddl_parser_context* grddl_parser,
                      raptor_uri* uri)
{
  if(!raptor_grddl_seen_uri(grddl_parser, uri)) {
    raptor_sequence* seq=grddl_parser->visited_uris;
    raptor_sequence_push(seq, raptor_uri_copy(uri));
  }
}


static void
raptor_grddl_parse_uri_write_bytes(raptor_www* www,
                                   void *userdata, const void *ptr,
                                   size_t size, size_t nmemb)
{
  raptor_parser* rdf_parser=(raptor_parser*)userdata;
  int len=size*nmemb;

  if(raptor_parse_chunk(rdf_parser, (unsigned char*)ptr, len, 0))
    raptor_www_abort(www, "Parsing failed");
}


static raptor_sequence*
raptor_grddl_run_xpath_match(raptor_parser* rdf_parser,
                             xmlDocPtr doc,
                             const xmlChar* xpathExpr,
                             int flags)
{
  raptor_grddl_parser_context* grddl_parser;
  /* Evaluate xpath expression */
  xmlXPathObjectPtr xpathObj=NULL;
  raptor_sequence* seq=NULL;
  xmlNodeSetPtr nodes;
  int i;
  
  grddl_parser=(raptor_grddl_parser_context*)rdf_parser->context;

  seq=raptor_new_sequence((raptor_sequence_free_handler*)raptor_free_uri, (raptor_sequence_print_handler*)raptor_sequence_print_uri);

  /* Evaluate xpath expression */
  xpathObj = xmlXPathEvalExpression(xpathExpr,
                                    grddl_parser->xpathCtx);
  if(!xpathObj) {
    raptor_parser_error(rdf_parser,
                        "Unable to evaluate XPath expression \"%s\"",
                        xpathExpr);
    raptor_free_sequence(seq); seq=NULL;
    goto cleanup_xpath_match;
  }

  nodes=xpathObj->nodesetval;
  if(!nodes || xmlXPathNodeSetIsEmpty(nodes)) {
    RAPTOR_DEBUG3("No match found with XPath expression \"%s\" over '%s'\n",
                  xpathExpr, raptor_uri_as_string(rdf_parser->base_uri));
    raptor_free_sequence(seq); seq=NULL;
    goto cleanup_xpath_match;
  }

  RAPTOR_DEBUG3("Found match with XPath expression \"%s\" over '%s'\n",
                xpathExpr, raptor_uri_as_string(rdf_parser->base_uri));
  
  for(i=0; i < xmlXPathNodeSetGetLength(nodes); i++) {
    xmlNodePtr node=nodes->nodeTab[i];
    const unsigned char* uri_string;
    xmlChar *base_uri_string;
    raptor_uri* base_uri=NULL;
    raptor_uri* uri=NULL;
    
    if(node->type != XML_ATTRIBUTE_NODE && 
       node->type != XML_ELEMENT_NODE) {
      raptor_parser_error(rdf_parser, "Got unexpected node type %d",
                          node->type);
      continue;
    }
    
    /* returns base URI or NULL - must be freed with xmlFree() */
    base_uri_string=xmlNodeGetBase(doc, node);
    
    if(node->type == XML_ATTRIBUTE_NODE)
      uri_string=(const unsigned char*)node->children->content;
    else /* XML_ELEMENT_NODE */
      uri_string=(const unsigned char*)node->ns->href;
    
    if(base_uri_string) {
      base_uri=raptor_new_uri(base_uri_string);
      xmlFree(base_uri_string);
      RAPTOR_DEBUG2("Got XML base URI '%s'\n", raptor_uri_as_string(base_uri));
    } else if(rdf_parser->base_uri)
      base_uri=raptor_uri_copy(rdf_parser->base_uri);
    else
      base_uri=NULL;
    
    if(flags & MATCH_IS_VALUE_LIST) {
      char *start;
      char *end;
      char* buffer;
      size_t list_len=strlen((const char*)uri_string);
      
      buffer=(char*)RAPTOR_MALLOC(cstring, list_len+1);
      strncpy(buffer, (const char*)uri_string, list_len+1);
      
      for(start=end=buffer; end; start=end+1) {
        end=strchr(start, ' ');
        if(end)
          *end='\0';
        
        if(start == end)
          continue;
        
        RAPTOR_DEBUG2("Got list URI '%s'\n", start);
        
        uri=raptor_new_uri_relative_to_base(base_uri,
                                            (const unsigned char*)start);
        if(flags & MATCH_IS_PROFILE &&
           !strcmp((const char*)raptor_uri_as_string(uri),
                   "http://www.w3.org/2003/g/data-view'")) {
          raptor_free_uri(uri);
          continue;
        }

        raptor_sequence_push(seq, uri);
      }
      RAPTOR_FREE(cstring, buffer);
    } else {
      uri=raptor_new_uri_relative_to_base(base_uri, uri_string);
      raptor_sequence_push(seq, uri);
    }
    
    if(base_uri)
      raptor_free_uri(base_uri);
  }

  cleanup_xpath_match:
  if(xpathObj)
    xmlXPathFreeObject(xpathObj);
  
  return seq;
}


static int
raptor_grddl_run_recursive(raptor_parser* rdf_parser, raptor_uri* uri)
{
  raptor_grddl_parser_context* grddl_parser;
  const char *parser_name="grddl";

  grddl_parser=(raptor_grddl_parser_context*)rdf_parser->context;

  if(raptor_grddl_seen_uri(grddl_parser, uri))
    return 0;
  
  if(raptor_grddl_ensure_internal_parser(rdf_parser, parser_name, 1))
    return 1;
  
  RAPTOR_DEBUG2("Running recursive GRDDL operation on URI '%s'\n",
                raptor_uri_as_string(uri));
  
  raptor_grddl_parser_add_parent(grddl_parser->internal_parser, grddl_parser);
  
  if(raptor_start_parse(grddl_parser->internal_parser, uri))
    return 1;
  
  if(raptor_grddl_fetch_uri(grddl_parser->internal_parser,
                            uri,
                            raptor_grddl_parse_uri_write_bytes,
                            grddl_parser->internal_parser))
    return 1;
  
  raptor_parse_chunk(grddl_parser->internal_parser, NULL, 0, 1);

  return 0;
}


static int
raptor_grddl_parse_chunk(raptor_parser* rdf_parser,
                         const unsigned char *s, size_t len,
                         int is_end)
{
  raptor_grddl_parser_context* grddl_parser=(raptor_grddl_parser_context*)rdf_parser->context;
  int i;
  int ret=0;
  const unsigned char* uri_string;
  raptor_uri* uri;
  /* XML document DOM */
  xmlDocPtr doc;
  int expri;

  if(!grddl_parser->ctxt) {
    uri_string=raptor_uri_as_string(rdf_parser->base_uri);

    /* first time, so init context with first read bytes */
    grddl_parser->ctxt = xmlCreatePushParserCtxt(NULL, NULL,
                                                (const char*)s, len,
                                                (const char*)uri_string);
    if(!grddl_parser->ctxt) {
      raptor_parser_error(rdf_parser, "Failed to create XML parser");
      return 1;
    }

    grddl_parser->ctxt->replaceEntities = 1;
    grddl_parser->ctxt->loadsubset = 1;

    if(is_end)
      xmlParseChunk(grddl_parser->ctxt, (const char*)s, 0, is_end);
  } else if((s && len) || is_end)
    xmlParseChunk(grddl_parser->ctxt, (const char*)s, len, is_end);

  if(!is_end)
    return 0;

  raptor_grddl_done_uri(grddl_parser, rdf_parser->base_uri);


  doc=grddl_parser->ctxt->myDoc;
  if(!doc) {
    raptor_parser_error(rdf_parser, "Failed to create XML DOM for document");
    return 1;
  }
  

  /* Work out if there is a root namespace URI */
  if(1) {
    xmlNodePtr xnp;
    xmlNsPtr rootNs = NULL;
    const unsigned char* ns_uri_string=NULL;

    xnp = xmlDocGetRootElement(doc);
    if(xnp) {
      rootNs = xnp->ns;
      if(rootNs)
        ns_uri_string = (const unsigned char*)(rootNs->href);
    }
    
    if(ns_uri_string) {
      int n;
      
      RAPTOR_DEBUG2("Root namespace URI is %s\n", ns_uri_string);
      for(n=0; grddl_namespace_uris_ignore_list[n]; n++) {
        if(!strcmp(grddl_namespace_uris_ignore_list[n],
                   (const char*)ns_uri_string)) {
          /* ignore this namespace */
          RAPTOR_DEBUG2("Ignoring GRDDL for namespace URI '%s'\n",
                        ns_uri_string);
          ns_uri_string=NULL;
          break;
        }
      }
      if(ns_uri_string) {
        grddl_parser->root_ns_uri=raptor_new_uri_relative_to_base(rdf_parser->base_uri, 
                                                                  ns_uri_string);
        RAPTOR_DEBUG2("Using GRDDL namespace URI '%s'\n",
                      raptor_uri_as_string(grddl_parser->root_ns_uri));
        raptor_grddl_run_recursive(rdf_parser, grddl_parser->root_ns_uri);
      }
      
    }
  }

  /* Always put something at the start of the list even if NULL 
   * so later it can be searched for in output triples
   */
  raptor_sequence_push(grddl_parser->profile_uris, 
                       raptor_uri_copy(grddl_parser->root_ns_uri));
        

  /* Create the XPath evaluation context */
  grddl_parser->xpathCtx = xmlXPathNewContext(doc);
  if(!grddl_parser->xpathCtx) {
    raptor_parser_error(rdf_parser,
                        "Failed to create XPath context for document");
    return 1;
  }

  xmlXPathRegisterNs(grddl_parser->xpathCtx,
                     (const xmlChar*)"html",
                     (const xmlChar*)"http://www.w3.org/1999/xhtml");
  xmlXPathRegisterNs(grddl_parser->xpathCtx,
                     (const xmlChar*)"dataview",
                     (const xmlChar*)"http://www.w3.org/2003/g/data-view#");

  /* Try <head profile> URIs */
  if(1) {
    raptor_sequence* result;
    result=raptor_grddl_run_xpath_match(rdf_parser, doc, 
                                        (const xmlChar*)"/html:html/html:head/@profile",
                                        MATCH_IS_VALUE_LIST | MATCH_IS_PROFILE);
    if(result) {
      RAPTOR_DEBUG2("Found %d <head profile> URIs \n",
                    raptor_sequence_size(result));

      /* Recursive GRDDL through all the <head profile> URIs */
      for(i=0; i < raptor_sequence_size(result); i++) {
        uri=raptor_sequence_get_at(result, i);
        if(uri) {
          RAPTOR_DEBUG2("Processing <head profile> URI %s\n",
                        raptor_uri_as_string(uri));
          ret=raptor_grddl_run_recursive(rdf_parser, uri);
        }
      }

      /* Store profile URIs for later */
      raptor_sequence_join(grddl_parser->profile_uris, result);

      raptor_free_sequence(result);
    }
  }
  
  
  /* Try all XPaths */
  for(expri=0; match_table[expri].xpath; expri++) {
    raptor_sequence* result;
    result=raptor_grddl_run_xpath_match(rdf_parser, doc, 
                                        match_table[expri].xpath,
                                        match_table[expri].flags);
    if(result) {
      if(match_table[expri].xslt_sheet_uri) {
        /* Ignore what matched, use a hardcoded XSLT URI */
        uri_string=match_table[expri].xslt_sheet_uri;
        RAPTOR_DEBUG2("Using hard-coded XSLT URI '%s'\n",  uri_string);
        
        uri=raptor_new_uri_relative_to_base(rdf_parser->base_uri, uri_string);
        raptor_sequence_set_at(result, 0, uri);
      }
      
      raptor_sequence_join(grddl_parser->doc_transform_uris, result);
      raptor_free_sequence(result);
    }

    
    if(rdf_parser->failed)
      break;

  } /* end XPath expression loop */
  
  if(rdf_parser->failed)
    return 1;


  /* Apply all transformation URIs seen */
  for(i=0; i < raptor_sequence_size(grddl_parser->doc_transform_uris); i++) {
    uri=raptor_sequence_unshift(grddl_parser->doc_transform_uris);
    RAPTOR_DEBUG2("Running transformation URI '%s'\n",
                  raptor_uri_as_string(uri));
    ret=raptor_grddl_run_grddl_transform_uri(rdf_parser, uri, doc);
    raptor_free_uri(uri);
    if(ret)
      break;
  }

  
  return (ret != 0);
}


static int
raptor_grddl_parse_recognise_syntax(raptor_parser_factory* factory,
                                    const unsigned char *buffer, size_t len,
                                    const unsigned char *identifier,
                                    const unsigned char *suffix,
                                    const char *mime_type)
{
  int score= 0;
  
  if(suffix) {
    if(!strcmp((const char*)suffix, "xhtml"))
      score=7;
    if(!strcmp((const char*)suffix, "html"))
      score=2;
  }
  
  if(identifier) {
    if(strstr((const char*)identifier, "xhtml"))
      score+=5;
  }
  
  return score;
}


static void
raptor_grddl_parser_register_factory(raptor_parser_factory *factory) 
{
  factory->context_length     = sizeof(raptor_grddl_parser_context);
  
  factory->need_base_uri = 1;
  
  factory->init      = raptor_grddl_parse_init;
  factory->terminate = raptor_grddl_parse_terminate;
  factory->start     = raptor_grddl_parse_start;
  factory->chunk     = raptor_grddl_parse_chunk;
  factory->recognise_syntax = raptor_grddl_parse_recognise_syntax;

  raptor_parser_factory_add_mime_type(factory, "text/html", 2);
  raptor_parser_factory_add_mime_type(factory, "application/html+xml", 2);
}


void
raptor_init_parser_grddl(void)
{
  raptor_parser_register_factory("grddl", 
                                 "Gleaning Resource Descriptions from Dialects of Languages",
                                 &raptor_grddl_parser_register_factory);

  xsltInit();
}


void
raptor_terminate_parser_grddl(void)
{
  xsltCleanupGlobals();
}
