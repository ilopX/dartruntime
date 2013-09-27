// Copyright (c) 2012, the Dart project authors.  Please see the AUTHORS file
// for details. All rights reserved. Use of this source code is governed by a
// BSD-style license that can be found in the LICENSE file.

// TODO(srdjan): fix limitations.
// - shift amount must be a Smi.
class _IntegerImplementation {
  factory _IntegerImplementation._uninstantiable() {
    throw new UnsupportedError(
        "_IntegerImplementation can only be allocated by the VM");
  }
  num operator +(num other) {
    return other._addFromInteger(this);
  }
  num operator -(num other) {
    return other._subFromInteger(this);
  }
  num operator *(num other) {
    return other._mulFromInteger(this);
  }
  num operator ~/(num other) {
    if ((other is int) && (other == 0)) {
      throw const IntegerDivisionByZeroException();
    }
    return other._truncDivFromInteger(this);
  }
  num operator /(num other) {
    return this.toDouble() / other.toDouble();
  }
  num operator %(num other) {
    if ((other is int) && (other == 0)) {
      throw const IntegerDivisionByZeroException();
    }
    return other._moduloFromInteger(this);
  }
  int operator -() {
    return 0 - this;
  }
  int operator &(int other) {
    return other._bitAndFromInteger(this);
  }
  int operator |(int other) {
    return other._bitOrFromInteger(this);
  }
  int operator ^(int other) {
    return other._bitXorFromInteger(this);
  }
  num remainder(num other) {
    return other._remainderFromInteger(this);
  }
  int _bitAndFromInteger(int other) native "Integer_bitAndFromInteger";
  int _bitOrFromInteger(int other) native "Integer_bitOrFromInteger";
  int _bitXorFromInteger(int other) native "Integer_bitXorFromInteger";
  int _addFromInteger(int other) native "Integer_addFromInteger";
  int _subFromInteger(int other) native "Integer_subFromInteger";
  int _mulFromInteger(int other) native "Integer_mulFromInteger";
  int _truncDivFromInteger(int other) native "Integer_truncDivFromInteger";
  int _moduloFromInteger(int other) native "Integer_moduloFromInteger";
  int _remainderFromInteger(int other) {
    return other - (other ~/ this) * this;
  }
  int operator >>(int other) {
    return other._shrFromInt(this);
  }
  int operator <<(int other) {
    return other._shlFromInt(this);
  }
  bool operator <(num other) {
    return other > this;
  }
  bool operator >(num other) {
    return other._greaterThanFromInteger(this);
  }
  bool operator >=(num other) {
    return (this == other) ||  (this > other);
  }
  bool operator <=(num other) {
    return (this == other) || (this < other);
  }
  bool _greaterThanFromInteger(int other)
      native "Integer_greaterThanFromInteger";
  bool operator ==(other) {
    if (other is num) {
      return other._equalToInteger(this);
    }
    return false;
  }
  bool _equalToInteger(int other) native "Integer_equalToInteger";
  int abs() {
    return this < 0 ? -this : this;
  }
  bool get isEven => ((this & 1) == 0);
  bool get isOdd => !isEven;
  bool get isNaN => false;
  bool get isNegative => this < 0;
  bool get isInfinite => false;

  int toUnsigned(int width) {
    return this & ((1 << width) - 1);
  }

  int toSigned(int width) {
    // The value of binary number weights each bit by a power of two.  The
    // twos-complement value weights the sign bit negatively.  We compute the
    // value of the negative weighting by isolating the sign bit with the
    // correct power of two weighting and subtracting it from the value of the
    // lower bits.
    int signMask = 1 << (width - 1);
    return (this & (signMask - 1)) - (this & signMask);
  }

  int compareTo(num other) {
    final int EQUAL = 0, LESS = -1, GREATER = 1;
    if (other is double) {
      // TODO(floitsch): the following locals should be 'const'.
      int MAX_EXACT_INT_TO_DOUBLE = 9007199254740992;  // 2^53.
      int MIN_EXACT_INT_TO_DOUBLE = -MAX_EXACT_INT_TO_DOUBLE;
      double d = other;
      if (d.isInfinite) {
        return d == double.NEGATIVE_INFINITY ? GREATER : LESS;
      }
      if (d.isNaN) {
        return LESS;
      }
      if (MIN_EXACT_INT_TO_DOUBLE <= this && this <= MAX_EXACT_INT_TO_DOUBLE) {
        // Let the double implementation deal with -0.0.
        return -(d.compareTo(this.toDouble()));
      } else {
        // If abs(other) > MAX_EXACT_INT_TO_DOUBLE, then other has an integer
        // value (no bits below the decimal point).
        other = d.toInt();
      }
    }
    if (this < other) {
      return LESS;
    } else if (this > other) {
      return GREATER;
    } else {
      return EQUAL;
    }
  }

  int round() { return this; }
  int floor() { return this; }
  int ceil() { return this; }
  int truncate() { return this; }

  double roundToDouble() { return this.toDouble(); }
  double floorToDouble() { return this.toDouble(); }
  double ceilToDouble() { return this.toDouble(); }
  double truncateToDouble() { return this.toDouble(); }

  num clamp(num lowerLimit, num upperLimit) {
    if (lowerLimit is! num) throw new ArgumentError(lowerLimit);
    if (upperLimit is! num) throw new ArgumentError(upperLimit);

    // Special case for integers.
    if (lowerLimit is int && upperLimit is int) {
      if (lowerLimit > upperLimit) {
        throw new ArgumentError(lowerLimit);
      }
      if (this < lowerLimit) return lowerLimit;
      if (this > upperLimit) return upperLimit;
      return this;
    }
    // Generic case involving doubles.
    if (lowerLimit.compareTo(upperLimit) > 0) {
      throw new ArgumentError(lowerLimit);
    }
    if (lowerLimit.isNaN) return lowerLimit;
    // Note that we don't need to care for -0.0 for the lower limit.
    if (this < lowerLimit) return lowerLimit;
    if (this.compareTo(upperLimit) > 0) return upperLimit;
    return this;
  }

  int toInt() { return this; }
  double toDouble() { return new _Double.fromInteger(this); }

  String toStringAsFixed(int fractionDigits) {
    return this.toDouble().toStringAsFixed(fractionDigits);
  }
  String toStringAsExponential([int fractionDigits]) {
    return this.toDouble().toStringAsExponential(fractionDigits);
  }
  String toStringAsPrecision(int precision) {
    return this.toDouble().toStringAsPrecision(precision);
  }

  static const _digits = "0123456789abcdefghijklmnopqrstuvwxyz";

  String toRadixString(int radix) {
    if (radix is! int || radix < 2 || radix > 36) {
      throw new ArgumentError(radix);
    }
    if (radix & (radix - 1) == 0) {
      return _toPow2String(this, radix);
    }
    if (radix == 10) return this.toString();
    final bool isNegative = this < 0;
    int value = isNegative ? -this : this;
    List temp = new List();
    do {
      int digit = value % radix;
      value ~/= radix;
      temp.add(_digits.codeUnitAt(digit));
    } while (value > 0);
    if (isNegative) temp.add(0x2d);  // '-'.

    _OneByteString string = _OneByteString._allocate(temp.length);
    for (int i = 0, j = temp.length; j > 0; i++) {
      string._setAt(i, temp[--j]);
    }
    return string;
  }

  static String _toPow2String(value, radix) {
    if (value == 0) return "0";
    assert(radix & (radix - 1) == 0);
    var negative = value < 0;
    var bitsPerDigit = radix.bitLength - 1;
    var length = 0;
    if (negative) {
      value = -value;
      length = 1;
    }
    // Integer division, rounding up, to find number of _digits.
    length += (value.bitLength + bitsPerDigit - 1) ~/ bitsPerDigit;
    _OneByteString string = _OneByteString._allocate(length);
    string._setAt(0, 0x2d);  // '-'. Is overwritten if not negative.
    var mask = radix - 1;
    do {
      string._setAt(--length, _digits.codeUnitAt(value & mask));
      value >>= bitsPerDigit;
    } while (value > 0);
    return string;
  }

  _leftShiftWithMask32(count, mask)  native "Integer_leftShiftWithMask32";
}

class _Smi extends _IntegerImplementation implements int {
  factory _Smi._uninstantiable() {
    throw new UnsupportedError(
        "_Smi can only be allocated by the VM");
  }
  int get _identityHashCode {
    return this;
  }
  int operator ~() native "Smi_bitNegate";
  int get bitLength native "Smi_bitLength";

  int _shrFromInt(int other) native "Smi_shrFromInt";
  int _shlFromInt(int other) native "Smi_shlFromInt";

  String toString() {
    if (this == 0) return "0";
    var reversed = _toStringBuffer;
    var negative = false;
    var val = this;
    int index = 0;

    if (this < 0) {
      negative = true;
      // Handle the first digit as negative to avoid negating the minimum
      // smi, for which the negation is not a smi.
      int digit = -(val.remainder(10));
      reversed[index++] = digit + 0x30;
      val = -(val ~/ 10);
    }

    while (val > 0) {
      int digit = val % 10;
      reversed[index++] = (digit + 0x30);
      val = val ~/ 10;
    }
    if (negative) reversed[index++] = 0x2D;  // '-'.

    _OneByteString string = _OneByteString._allocate(index);
    for (int i = 0, j = index; i < index; i++) {
      string._setAt(i, reversed[--j]);
    }
    return string;
  }
}

// Reusable buffer used by smi.toString.
List _toStringBuffer = new Uint8List(20);

// Represents integers that cannot be represented by Smi but fit into 64bits.
class _Mint extends _IntegerImplementation implements int {
  factory _Mint._uninstantiable() {
    throw new UnsupportedError(
        "_Mint can only be allocated by the VM");
  }
  int get _identityHashCode {
    return this;
  }
  int operator ~() native "Mint_bitNegate";
  int get bitLength native "Mint_bitLength";

  // Shift by mint exceeds range that can be handled by the VM.
  int _shrFromInt(int other) {
    if (other < 0) {
      return -1;
    } else {
      return 0;
    }
  }
  int _shlFromInt(int other) native "Mint_shlFromInt";
}

// A number that can be represented as Smi or Mint will never be represented as
// Bigint.
class _Bigint extends _IntegerImplementation implements int {
  factory _Bigint._uninstantiable() {
    throw new UnsupportedError(
        "_Bigint can only be allocated by the VM");
  }
  int get _identityHashCode {
    return this;
  }
  int operator ~() native "Bigint_bitNegate";
  int get bitLength native "Bigint_bitLength";

  // Shift by bigint exceeds range that can be handled by the VM.
  int _shrFromInt(int other) {
    if (other < 0) {
      return -1;
    } else {
      return 0;
    }
  }
  int _shlFromInt(int other) native "Bigint_shlFromInt";

  int pow(int exponent) {
    throw "Bigint.pow not implemented";
  }
}
