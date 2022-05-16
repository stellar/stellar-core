///////////////////////////////////////////////////////////////////////////////
//
// Copyright (c) 2015 Microsoft Corporation. All rights reserved.
//
// This code is licensed under the MIT License (MIT).
//
// THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
// IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
// FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
// AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
// LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
// OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
// THE SOFTWARE.
//
///////////////////////////////////////////////////////////////////////////////

// This code was copied from the ISO C++ Core Guidelines guildeline support
// library (GSL) implementation from Microsoft.
//
// https://github.com/Microsoft/GSL
//
// We therefore keep it in the same 'gsl' namespace it appeared in in the
// original file.

#include <utility>
namespace gsl
{

// final_action allows you to ensure something gets run at the end of a scope
template <class F> class final_action
{
  public:
    explicit final_action(F f) noexcept : f_(std::move(f))
    {
    }

    final_action(final_action&& other) noexcept
        : f_(std::move(other.f_)), invoke_(std::exchange(other.invoke_, false))
    {
    }

    final_action(const final_action&) = delete;
    final_action& operator=(const final_action&) = delete;
    final_action& operator=(final_action&&) = delete;

    ~final_action() noexcept
    {
        if (invoke_)
            f_();
    }

  private:
    F f_;
    bool invoke_{true};
};

// finally() - convenience function to generate a final_action
template <class F>
final_action<F> finally(const F& f) noexcept
{
    return final_action<F>(f);
}

template <class F>
final_action<F> finally(F&& f) noexcept
{
    return final_action<F>(std::forward<F>(f));
}

}
