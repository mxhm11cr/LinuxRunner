// Color conversion matrices exercise scalar fused FP decoding in Qt's inverse.
// Run with the guest's unmodified Qt: c++ -O2 -fPIC qt_matrix_inverse_contract.cpp
// $(pkg-config --cflags --libs Qt6Gui) -o qt-matrix && ./qt-matrix
#include <QMatrix4x4>
#include <cmath>
#include <cstdio>

int main() {
    const QMatrix4x4 matrix(.4123908f, .3575843f, .1804808f, 0,
                           .212639f, .7151687f, .0721923f, 0,
                           .0193308f, .1191948f, .9505322f, 0,
                           0, 0, 0, 1);
    bool invertible = false;
    const auto inverse = matrix.inverted(&invertible);
    const auto product = matrix * inverse;
    unsigned failures = !invertible;
    for (int row = 0; row < 4; row++) {
        for (int column = 0; column < 4; column++) {
            const float actual = product(row, column);
            if (!std::isfinite(actual) ||
                std::abs(actual - (row == column ? 1.f : 0.f)) > 1e-4f) {
                std::printf("QT_MATRIX FAIL row=%d column=%d actual=%.9g\n",
                            row, column, actual);
                failures++;
            }
        }
    }
    std::printf("QT_MATRIX %s failures=%u\n", failures ? "FAIL" : "PASS", failures);
    return failures ? 1 : 0;
}
