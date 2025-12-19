// TestSymbolsDll.cpp - Implementation of test DLL with mixed PUBLIC and PRIVATE symbols

#include "TestSymbolsDll.h"
#include <cstring>
#include <cmath>

// ============================================================================
// Shape class implementation (PUBLIC)
// ============================================================================

void Shape::Draw() const
{
    // Base implementation
}

void Shape::SetName(const char* name)
{
    if (name)
    {
        strncpy_s(m_name, sizeof(m_name), name, _TRUNCATE);
    }
}

const char* Shape::GetName() const
{
    return m_name;
}

// ============================================================================
// Circle class implementation (PUBLIC)
// ============================================================================

Circle::Circle(double radius) : m_radius(radius)
{
    SetName("Circle");
}

Circle::~Circle()
{
}

double Circle::GetArea() const
{
    return 3.14159 * m_radius * m_radius;
}

double Circle::GetPerimeter() const
{
    return 2 * 3.14159 * m_radius;
}

void Circle::Draw() const
{
    // Circle-specific drawing
}

double Circle::GetRadius() const
{
    return m_radius;
}

// ============================================================================
// InternalHelper class implementation (PRIVATE - not exported)
// ============================================================================

int InternalHelper::Calculate(int x)
{
    return x * x + x;
}

double InternalHelper::ProcessData(double value)
{
    return sqrt(value) * 2.0;
}

// ============================================================================
// PUBLIC function implementations
// ============================================================================

int Add(int a, int b)
{
    return a + b;
}

double Add(double a, double b)
{
    return a + b;
}

int Add(int a, int b, int c)
{
    return a + b + c;
}

// ============================================================================
// PRIVATE function implementations (not exported)
// ============================================================================

int InternalAdd(int a, int b)
{
    return a + b + 1;
}

void InternalProcess()
{
    // Internal processing
    int result = InternalHelper::Calculate(42);
    double data = InternalHelper::ProcessData(result);
}

// ============================================================================
// Complex class implementation (PUBLIC)
// ============================================================================

Complex::Complex(double real, double imag) : m_real(real), m_imag(imag)
{
}

Complex::~Complex()
{
}

Complex Complex::operator+(const Complex& other) const
{
    return Complex(m_real + other.m_real, m_imag + other.m_imag);
}

Complex Complex::operator*(const Complex& other) const
{
    return Complex(
        m_real * other.m_real - m_imag * other.m_imag,
        m_real * other.m_imag + m_imag * other.m_real
    );
}

bool Complex::operator==(const Complex& other) const
{
    return m_real == other.m_real && m_imag == other.m_imag;
}

double Complex::GetReal() const
{
    return m_real;
}

double Complex::GetImag() const
{
    return m_imag;
}

// ============================================================================
// Template instantiations (PUBLIC)
// ============================================================================

// Explicit template instantiation with export
template TESTSYMBOLS_API int Max<int>(int, int);
template TESTSYMBOLS_API double Max<double>(double, double);

