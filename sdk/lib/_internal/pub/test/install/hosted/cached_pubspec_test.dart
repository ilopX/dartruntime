// Copyright (c) 2012, the Dart project authors.  Please see the AUTHORS file
// for details. All rights reserved. Use of this source code is governed by a
// BSD-style license that can be found in the LICENSE file.

library pub_tests;

import 'package:path/path.dart' as path;
import 'package:scheduled_test/scheduled_test.dart';

import '../../../lib/src/io.dart';
import '../../descriptor.dart' as d;
import '../../test_pub.dart';

main() {
  initConfig();

  integration('does not request a pubspec for a cached package', () {
    servePackages([packageMap("foo", "1.2.3")]);

    d.appDir({"foo": "1.2.3"}).create();

    // Run install once so it gets cached.
    pubInstall();

    // Clear the cache. We don't care about anything that was served during
    // the initial install.
    getRequestedPaths();

    d.cacheDir({"foo": "1.2.3"}).validate();
    d.packagesDir({"foo": "1.2.3"}).validate();

    // Run the solver again now that it's cached.
    pubInstall();

    // The update should not have requested the pubspec since it's installed
    // locally already.
    getRequestedPaths().then((paths) {
      expect(paths, isNot(contains("packages/foo/versions/1.2.3.yaml")));
    });
  });
}
