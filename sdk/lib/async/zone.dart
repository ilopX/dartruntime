// Copyright (c) 2013, the Dart project authors.  Please see the AUTHORS file
// for details. All rights reserved. Use of this source code is governed by a
// BSD-style license that can be found in the LICENSE file.

part of dart.async;

/**
 * A Zone represents the asynchronous version of a dynamic extent. Asynchronous
 * callbacks are executed in the zone they have been queued in. For example,
 * the callback of a `future.then` is executed in the same zone as the one where
 * the `then` was invoked.
 */
abstract class _Zone {
  /// The currently running zone.
  static _Zone _current = new _DefaultZone();

  static _Zone get current => _current;

  void handleUncaughtError(error);

  /**
   * Returns true if `this` and [otherZone] are in the same error zone.
   */
  bool inSameErrorZone(_Zone otherZone);

  /**
   * Returns a zone for reentry in the zone.
   *
   * The returned zone is equivalent to `this` (and frequently is indeed
   * `this`).
   *
   * The main purpose of this method is to allow `this` to attach debugging
   * information to the returned zone.
   */
  _Zone fork();

  /**
   * Tells the zone that it needs to wait for one more callback before it is
   * done.
   *
   * Use [executeCallback] or [unexpectCallback] when the callback is executed
   * (or canceled).
   */
  void expectCallback();

  /**
   * Tells the zone not to wait for a callback anymore.
   *
   * Prefer calling [executeCallback], instead. This method is mostly useful
   * for repeated callbacks (for example with [Timer.periodic]). In this case
   * one should should call [expectCallback] when the repeated callback is
   * initiated, and [unexpectCallback] when the [Timer] is canceled.
   */
  void unexpectCallback();

  /**
   * Executes the given callback in this zone.
   *
   * Decrements the number of callbacks this zone is waiting for (see
   * [expectCallback]).
   */
  void executeCallback(void fun());

  /**
   * Same as [executeCallback] but catches uncaught errors and gives them to
   * [handleUncaughtError].
   */
  void executeCallbackGuarded(void fun());

  /**
   * Same as [executeCallback] but does not decrement the number of
   * callbacks this zone is waiting for (see [expectCallback]).
   */
  void executePeriodicCallback(void fun());

  /**
   * Same as [executePeriodicCallback] but catches uncaught errors and gives
   * them to [handleUncaughtError].
   */
  void executeGuardedPeriodicCallback(void fun());

  /**
   * Runs [fun] asynchronously in this zone.
   */
  void runAsync(void fun());

  /**
   * Creates a Timer where the callback is executed in this zone.
   */
  Timer createTimer(Duration duration, void callback());

  /**
   * Creates a periodic Timer where the callback is executed in this zone.
   */
  Timer createPeriodicTimer(Duration duration, void callback(Timer timer));

  /**
   * The error zone is the one that is responsible for dealing with uncaught
   * errors. Errors are not allowed to cross zones with different error-zones.
   */
  _Zone get _errorZone;

  /**
   * Adds [child] as a child of `this`.
   *
   * This usually means that the [child] is in the asynchronous dynamic extent
   * of `this`.
   */
  void _addChild(_Zone child);

  /**
   * Removes [child] from `this`' children.
   *
   * This usually means that the [child] has finished executing and is done.
   */
  void _removeChild(_Zone child);
}

/**
 * Basic implementation of a [_Zone]. This class is intended for subclassing.
 */
class _ZoneBase implements _Zone {
  /// The parent zone. [null] if `this` is the default zone.
  final _Zone _parentZone;

  /// The children of this zone. A child's [_parentZone] is `this`.
  // TODO(floitsch): this should be a double-linked list.
  final List<_Zone> _children = <_Zone>[];

  /// The number of outstanding (asynchronous) callbacks. As long as the
  /// number is greater than 0 it means that the zone is not done yet.
  int _openCallbacks = 0;

  _ZoneBase(this._parentZone) {
    _parentZone._addChild(this);
  }

  _ZoneBase._defaultZone() : _parentZone = null {
    assert(this is _DefaultZone);
  }

  _Zone get _errorZone => _parentZone._errorZone;

  void handleUncaughtError(error) {
    _parentZone.handleUncaughtError(error);
  }

  bool inSameErrorZone(_Zone otherZone) => _errorZone == otherZone._errorZone;

  _Zone fork() => this;

  expectCallback() => _openCallbacks++;

  unexpectCallback() {
    _openCallbacks--;
    _checkIfDone();
  }

  /**
   * Cleans up this zone when it is done.
   *
   * This releases internal memore structures that are no longer necessary.
   *
   * A zone is done when its dynamic extent has finished executing and
   * there are no outstanding asynchronous callbacks.
   */
  _dispose() {
    if (_parentZone != null) {
      _parentZone._removeChild(this);
    }
  }

  /**
   * Checks if the zone is done and doesn't have any outstanding callbacks
   * anymore.
   *
   * This method is called when an operation has decremented the
   * outstanding-callback count, or when a child has been removed.
   */
  void _checkIfDone() {
    if (_openCallbacks == 0 && _children.isEmpty) {
      _dispose();
    }
  }

  /**
   * Executes the given callback in this zone.
   *
   * Decrements the open-callback counter and checks (after the call) if the
   * zone is done.
   */
  void executeCallback(void fun()) {
    _openCallbacks--;
    _runInZone(fun);
  }

  /**
   * Same as [executeCallback] but catches uncaught errors and gives them to
   * [handleUncaughtError].
   */
  void executeCallbackGuarded(void fun()) {
    _openCallbacks--;
    _runGuarded(fun);
  }

  /**
   * Same as [executeCallback] but doesn't decrement the open-callback counter.
   */
  void executePeriodicCallback(void fun()) {
    _runInZone(fun);
  }

  /**
   * Same as [executePeriodicCallback] but catches uncaught errors and gives
   * them to [handleUncaughtError].
   */
  void executeGuardedPeriodicCallback(void fun()) {
    _runGuarded(fun);
  }

  _runInZone(fun()) {
    if (identical(_Zone._current, this) && _openCallbacks != 0) return fun();

    _Zone oldZone = _Zone._current;
    _Zone._current = this;
    // While we are executing the function we don't want to have other
    // synchronous calls to think that they closed the zone. By incrementing
    // the _openCallbacks count we make sure that their test will fail.
    // As a side effect it will make nested calls faster since they are
    // (probably) in the same zone and have an _openCallbacks > 0.
    _openCallbacks++;
    try {
      return fun();
    } finally {
      _openCallbacks--;
      _Zone._current = oldZone;
      _checkIfDone();
    }
  }

  /**
   * Runs the function and catches uncaught errors.
   *
   * Uncaught errors are given to [handleUncaughtError].
   */
  _runGuarded(void fun()) {
    try {
      _runInZone(fun);
    } catch(e, s) {
      handleUncaughtError(_asyncError(e, s));
    }
  }

  runAsync(void fun()) {
    _openCallbacks++;
    _scheduleAsyncCallback(() {
      _openCallbacks--;
      _runGuarded(fun);
    });
  }

  Timer createTimer(Duration duration, void callback()) {
    return new _ZoneTimer(this, duration, callback);
  }

  Timer createPeriodicTimer(Duration duration, void callback(Timer timer)) {
    return new _PeriodicZoneTimer(this, duration, callback);
  }

  void _addChild(_Zone child) {
    _children.add(child);
  }

  void _removeChild(_Zone child) {
    assert(!_children.isEmpty);
    // Children are usually added and removed fifo or filo.
    if (identical(_children.last, child)) {
      _children.length--;
      _checkIfDone();
      return;
    }
    for (int i = 0; i < _children.length; i++) {
      if (identical(_children[i], child)) {
        _children[i] = _children[_children.length - 1];
        _children.length--;
        // No need to check for done, as otherwise _children.last above would
        // have triggered.
        assert(!_children.isEmpty);
        return;
      }
    }
    throw new ArgumentError(child);
  }
}

/**
 * The default-zone that conceptually surrounds the `main` function.
 */
class _DefaultZone extends _ZoneBase {
  _DefaultZone() : super._defaultZone();

  _Zone get _errorZone => this;

  handleUncaughtError(error) {
    _scheduleAsyncCallback(() {
      print("Uncaught Error: ${error}");
      var trace = getAttachedStackTrace(error);
      _attachStackTrace(error, null);
      if (trace != null) {
        print("Stack Trace:\n$trace\n");
      }
      throw error;
    });
  }
}

/**
 * A zone that can execute a callback (through a future) when the zone is dead.
 */
class _WaitForCompletionZone extends _ZoneBase {
  final Completer _doneCompleter = new Completer();

  _WaitForCompletionZone(_Zone parentZone) : super(parentZone);

  /**
   * Runs the given function asynchronously and returns a future that is
   * completed with `null` once the zone is done.
   */
  Future runWaitForCompletion(void fun()) {
    _runInZone(() {
      try {
        fun();
      } catch (e, s) {
        handleUncaughtError(_asyncError(e, s));
      }
    });
    return _doneCompleter.future;
  }

  _dispose() {
    super._dispose();
    _doneCompleter.complete();
  }

  String toString() => "WaitForCompletion ${super.toString()}";
}

/**
 * A zone that collects all uncaught errors and provides them in a stream.
 * The stream is closed when the zone is done.
 */
class _CatchErrorsZone extends _WaitForCompletionZone {
  final StreamController errorsController = new StreamController();

  Stream get errors => errorsController.stream;

  _CatchErrorsZone(_Zone parentZone) : super(parentZone);

  _Zone get _errorZone => this;

  handleUncaughtError(error) {
    errorsController.add(error);
  }

  Future runWaitForCompletion(void fun()) {
    super.runWaitForCompletion(fun).whenComplete(() {
      errorsController.close();
    });
  }

  String toString() => "WithErrors ${super.toString()}";
}

typedef void _TimerCallback();

/**
 * A [Timer] class that takes zones into account.
 */
class _ZoneTimer implements Timer {
  final _Zone _zone;
  final _TimerCallback _callback;
  Timer _timer;
  bool _isDone = false;

  _ZoneTimer(this._zone, Duration duration, this._callback) {
    _zone.expectCallback();
    _timer = _createTimer(duration, this.run);
  }

  void run() {
    _isDone = true;
    _zone.executeCallbackGuarded(_callback);
  }

  void cancel() {
    if (!_isDone) _zone.unexpectCallback();
    _isDone = true;
    _timer.cancel();
  }
}

typedef void _PeriodicTimerCallback(Timer timer);

/**
 * A [Timer] class for periodic callbacks that takes zones into account.
 */
class _PeriodicZoneTimer implements Timer {
  final _Zone _zone;
  final _PeriodicTimerCallback _callback;
  Timer _timer;
  bool _isDone = false;

  _PeriodicZoneTimer(this._zone, Duration duration, this._callback) {
    _zone.expectCallback();
    _timer = _createPeriodicTimer(duration, this.run);
  }

  void run(Timer timer) {
    assert(identical(_timer, timer));
    _zone.executeGuardedPeriodicCallback(() { _callback(this); });
  }

  void cancel() {
    if (!_isDone) _zone.unexpectCallback();
    _isDone = true;
    _timer.cancel();
  }
}

Stream catchErrors(void body()) {
  _CatchErrorsZone catchErrorsZone = new _CatchErrorsZone(_Zone._current);
  catchErrorsZone.runWaitForCompletion(body);
  return catchErrorsZone.errors;
}

Future waitForCompletion(void body()) {
  _WaitForCompletionZone zone = new _WaitForCompletionZone(_Zone._current);
  return zone.runWaitForCompletion(body);
}
