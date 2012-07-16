#!/usr/bin/python
# Copyright (c) 2011, the Dart project authors.  Please see the AUTHORS file
# for details. All rights reserved. Use of this source code is governed by a
# BSD-style license that can be found in the LICENSE file.

"""This is the entry point to create Dart APIs from the IDL database."""

import dartgenerator
import database
import logging.config
import multiemitter
import optparse
import os
import shutil
import subprocess
import sys
from generator import TypeRegistry
from htmlrenamer import HtmlRenamer
from systembase import GeneratorOptions
from systemfrog import FrogSystem
from systemhtml import HtmlInterfacesSystem, HtmlFrogSystem
from systeminterface import InterfacesSystem
from systemnative import NativeImplementationSystem
from templateloader import TemplateLoader

_logger = logging.getLogger('dartdomgenerator')

_webkit_renames = {
    # W3C -> WebKit name conversion
    # TODO(vsm): Maybe Store these renames in the IDLs.
    'ApplicationCache': 'DOMApplicationCache',
    'BarProp': 'BarInfo',
    'DedicatedWorkerGlobalScope': 'DedicatedWorkerContext',
    'FormData': 'DOMFormData',
    'Selection': 'DOMSelection',
    'SharedWorkerGlobalScope': 'SharedWorkerContext',
    'Window': 'DOMWindow',
    'WorkerGlobalScope': 'WorkerContext'}

def Generate(system_names, database_dir, use_database_cache, dom_output_dir,
             html_output_dir):
  current_dir = os.path.dirname(__file__)
  auxiliary_dir = os.path.join(current_dir, '..', 'src')
  template_dir = os.path.join(current_dir, '..', 'templates')

  generator = dartgenerator.DartGenerator()
  generator.LoadAuxiliary(auxiliary_dir)

  common_database = database.Database(database_dir)
  if use_database_cache:
    common_database.LoadFromCache()
  else:
    common_database.Load()

  generator.FilterMembersWithUnidentifiedTypes(common_database)
  webkit_database = common_database.Clone()

  # Generate Dart interfaces for the WebKit DOM.
  generator.FilterInterfaces(database = webkit_database,
                             or_annotations = ['WebKit', 'Dart'],
                             exclude_displaced = ['WebKit'],
                             exclude_suppressed = ['WebKit', 'Dart'])
  generator.RenameTypes(webkit_database, _webkit_renames, True)
  generator.FixEventTargets(webkit_database)

  def CreateGeneratorOptions(template_paths, conditions, type_registry, output_dir,
                             renamer=None):
    template_loader = TemplateLoader(template_dir, template_paths, conditions)
    return GeneratorOptions(
        template_loader, webkit_database, emitters, type_registry, renamer,
        output_dir)

  def Generate(system):
    generator.Generate(webkit_database, system,
                       super_database=common_database,
                       webkit_renames=_webkit_renames)

  emitters = multiemitter.MultiEmitter()

  for system_name in system_names:
    if system_name in ['htmlfrog', 'htmldartium']:
      renamer = HtmlRenamer(webkit_database)
      type_registry = TypeRegistry(webkit_database, renamer)
      if system_name == 'htmlfrog':
        options = CreateGeneratorOptions(
            ['html/frog', 'html/impl', 'html', ''],
            {'DARTIUM': False, 'FROG': True},
            type_registry, html_output_dir, renamer)
        backend = HtmlFrogSystem(options)
      else:
        options = CreateGeneratorOptions(
            ['dom/native', 'html/dartium', 'html/impl', ''],
            {'DARTIUM': True, 'FROG': False},
            type_registry, html_output_dir, renamer)
        backend = NativeImplementationSystem(options, auxiliary_dir)
      options = CreateGeneratorOptions(
          ['html/interface', 'html/impl', 'html', ''], {},
          type_registry, html_output_dir, renamer)
      html_system = HtmlInterfacesSystem(options, backend)
      Generate(html_system)
    else:
      type_registry = TypeRegistry(webkit_database)
      options = CreateGeneratorOptions(
          ['dom/interface', 'dom', ''], {}, type_registry, dom_output_dir)
      interface_system = InterfacesSystem(options)
      if system_name == 'dummy':
        options = CreateGeneratorOptions(
            ['dom/dummy', 'dom', ''], {}, type_registry, dom_output_dir)
        implementation_system = dartgenerator.DummyImplementationSystem(
            options)
      elif system_name == 'frog':
        options = CreateGeneratorOptions(
            ['dom/frog', 'dom', ''], {}, type_registry, dom_output_dir)
        implementation_system = FrogSystem(options)
      else:
        raise Exception('Unsupported system_name %s' % system_name)

      # Makes interface files available for listing in the library for the
      # implementation system.
      implementation_system._interface_system = interface_system
      Generate(interface_system)
      Generate(implementation_system)

  _logger.info('Flush...')
  emitters.Flush()

def GenerateSingleFile(systems):
  if 'frog' in systems:
    _logger.info('Copy dom_frog to frog/')
    subprocess.call(['cd ../generated ; '
                     '../../../tools/copy_dart.py ../frog dom_frog.dart'],
                    shell=True)

  if 'htmlfrog' in systems:
    _logger.info('Copy html_frog to ../html/frog/')
    subprocess.call(['cd ../../html/generated ; '
                     '../../../tools/copy_dart.py ../frog html_frog.dart'],
                    shell=True)

  if 'htmldartium' in systems:
    _logger.info('Copy html_dartium to ../html/dartium/')
    subprocess.call(['cd ../../html/generated ; '
                     '../../../tools/copy_dart.py ../dartium html_dartium.dart'],
                    shell=True)

  # Copy dummy DOM where dartc build expects it.
  if 'dummy' in systems:
    _logger.info('Copy dom_dummy to dom.dart')
    subprocess.call(['cd ../generated ; '
                     '../../../tools/copy_dart.py dummy dom_dummy.dart ;'
                     'cp dummy/dom_dummy.dart ../dom.dart'],
                    shell=True)

def main():
  parser = optparse.OptionParser()
  parser.add_option('--systems', dest='systems',
                    action='store', type='string',
                    default='frog,dummy,htmlfrog,htmldartium',
                    help='Systems to generate (frog, dummy, '
                         'htmlfrog, htmldartium)')
  parser.add_option('--output-dir', dest='output_dir',
                    action='store', type='string',
                    default=None,
                    help='Directory to put the generated files')
  parser.add_option('--use-database-cache', dest='use_database_cache',
                    action='store_true',
                    default=False,
                    help='''Use the cached database from the previous run to
                    improve startup performance''')
  (options, args) = parser.parse_args()

  current_dir = os.path.dirname(__file__)
  database_dir = os.path.join(current_dir, '..', 'database')
  logging.config.fileConfig(os.path.join(current_dir, 'logging.conf'))
  systems = options.systems.split(',')

  dom_output_dir = options.output_dir or os.path.join(current_dir,
      '../generated')
  html_output_dir = options.output_dir or os.path.join(current_dir,
      '../../html/generated')
  Generate(systems, database_dir, options.use_database_cache,
              dom_output_dir, html_output_dir)
  GenerateSingleFile(systems)

if __name__ == '__main__':
  sys.exit(main())
